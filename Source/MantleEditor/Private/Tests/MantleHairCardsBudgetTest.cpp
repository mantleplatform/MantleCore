// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairCards.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The hair triangle budget (#106): the cards kept are spread evenly and every section keeps at least one. Synthetic cards, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairCardsBudgetTestPrivate
{
	/** A card of Quads quads stacked along Z at (X, Y): 4 + 2 per extra quad vertices, 2 triangles per quad. */
	void BudgetTestCard(float X, float Y, int32 Quads, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		const uint32 Base = P.Num();
		for (int32 Q = 0; Q <= Quads; ++Q)
		{
			P.Add(FVector3f(X, Y, static_cast<float>(Q)));
			P.Add(FVector3f(X + 0.5f, Y, static_cast<float>(Q)));
		}
		for (int32 Q = 0; Q < Quads; ++Q)
		{
			const uint32 A = Base + 2 * Q;
			I.Append({A, A + 1, A + 3, A, A + 3, A + 2});
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsBudgetEvenTest,
	"Mantle.Editor.HairCards.BudgetKeepsCardsSpreadEvenly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsBudgetEvenTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCardsBudgetTestPrivate;
	// 20 x 20 cards 1 cm apart, 2 triangles each; a tenth of them fit.
	TArray<FVector3f> P; TArray<uint32> I;
	for (int32 Y = 0; Y < 20; ++Y) for (int32 X = 0; X < 20; ++X) BudgetTestCard(static_cast<float>(X) * 2.f, static_cast<float>(Y) * 2.f, 1, P, I);
	const TArray<int32> Kept = FMantleHairCards::SelectWholeCards(P, I, 80);
	TestEqual(TEXT("40 cards of 2 triangles fit 80"), Kept.Num(), 80);

	TArray<FVector2f> Centres;
	for (const int32 T : Kept) if (T % 2 == 0) Centres.Add(FVector2f(P[I[T * 3]].X + 0.25f, P[I[T * 3]].Y));
	float Worst = 0.f;
	for (int32 Y = 0; Y < 20; ++Y) for (int32 X = 0; X < 20; ++X)
	{
		float Nearest = TNumericLimits<float>::Max();
		for (const FVector2f& C : Centres) Nearest = FMath::Min(Nearest, FVector2f::Distance(C, FVector2f(X * 2.f + 0.25f, Y * 2.f)));
		Worst = FMath::Max(Worst, Nearest);
	}
	// 40 cards over a 38 x 38 cm patch is about 6 cm apart; no card may be more than 9 cm from a kept one (a random pick leaves holes twice that).
	TestTrue(FString::Printf(TEXT("no hole in the kept hair (worst %.1f cm)"), Worst), Worst <= 9.f);
	TestEqual(TEXT("the same call gives the same cards"), FMantleHairCards::SelectWholeCards(P, I, 80), Kept);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsBudgetBigCardTest,
	"Mantle.Editor.HairCards.BudgetSkipsACardThatDoesNotFitForASmallerOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsBudgetBigCardTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCardsBudgetTestPrivate;
	// One 40-triangle card in the middle of eight 2-triangle cards, budget 10: the big card never fits, the small ones do.
	TArray<FVector3f> P; TArray<uint32> I;
	BudgetTestCard(10.f, 10.f, 20, P, I);
	for (int32 K = 0; K < 8; ++K) BudgetTestCard(static_cast<float>(K) * 3.f, 0.f, 1, P, I);
	const TArray<int32> Kept = FMantleHairCards::SelectWholeCards(P, I, 10);
	TestTrue(TEXT("something is kept"), Kept.Num() > 0);
	TestTrue(TEXT("within the budget"), Kept.Num() <= 10);
	TestTrue(TEXT("the big card is not kept"), !Kept.Contains(0));
	TestEqual(TEXT("the smallest card is found"), FMantleHairCards::SmallestCardTriangles(P, I), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsBudgetSectionsTest,
	"Mantle.Editor.HairCards.BudgetGivesEverySectionItsSmallestCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsBudgetSectionsTest::RunTest(const FString& Parameters)
{
	bool bOver = true;
	// hair006-like: a small section whose share is below its first card gets that card, the big one pays for it.
	TArray<int32> Out = FMantleHairCards::AllocateSectionBudgets({20000, 100}, {40, 30}, 14000, bOver);
	TestFalse(TEXT("fits"), bOver);
	TestTrue(TEXT("the small section keeps at least its smallest card"), Out[1] >= 30);
	TestTrue(TEXT("the total is within the budget"), Out[0] + Out[1] <= 14000);

	// A pack that cannot fit at all: one smallest card per section, and it says so.
	Out = FMantleHairCards::AllocateSectionBudgets({20000, 20000}, {9000, 9000}, 14000, bOver);
	TestTrue(TEXT("over budget"), bOver);
	TestEqual(TEXT("first section keeps its smallest card"), Out[0], 9000);
	TestEqual(TEXT("second section keeps its smallest card"), Out[1], 9000);
	return true;
}

#endif
