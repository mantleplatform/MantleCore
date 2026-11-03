// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelItemMeasure.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWheelItemTestPrivate
{
	/** A rim 40 radius and 20 wide, axle on X: a ring of 16 points at each of two X (the barrel's lips) and a hub of 8 points at HubX. */
	TArray<FVector3f> MakeRim(const float HubX, const FVector3f Offset = FVector3f::ZeroVector)
	{
		TArray<FVector3f> Out;
		for (const float X : { -10.f, 10.f })
		{
			for (int32 Index = 0; Index < 16; ++Index)
			{
				const float Angle = 2.f * UE_PI * Index / 16.f;
				Out.Add(Offset + FVector3f(X, 40.f * FMath::Cos(Angle), 40.f * FMath::Sin(Angle)));
			}
		}
		for (int32 Index = 0; Index < 8; ++Index)
		{
			const float Angle = 2.f * UE_PI * Index / 8.f;
			Out.Add(Offset + FVector3f(HubX, 10.f * FMath::Cos(Angle), 10.f * FMath::Sin(Angle)));
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWheelItemMeasureTest,
	"Mantle.Wheel.Item.Measure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleWheelItemMeasureTest::RunTest(const FString& Parameters)
{
	using namespace MantleWheelItemTestPrivate;

	const FMantleWheelItemMeasure Good = FMantleWheelItem::Measure(MakeRim(5.f));
	TestTrue(TEXT("a rim measures"), Good.bOk);
	TestEqual(TEXT("radius, cm"), Good.ReferenceRadius, 40.0, 0.01);
	TestEqual(TEXT("width, cm"), Good.ReferenceWidth, 20.0, 0.01);
	TestTrue(TEXT("a centered rim has nothing to say"), Good.Problem.IsEmpty());

	// Which side is the face is not guessed: a hub toward either end (or none) measures the same.
	TestTrue(TEXT("a deep-dish rim, hub toward -X, is not refused"), FMantleWheelItem::Measure(MakeRim(-5.f)).bOk);
	TestTrue(TEXT("a flat face is not refused"), FMantleWheelItem::Measure(MakeRim(0.f)).bOk);

	// A rim off its axle or off-center is refused, with what to do, not fitted wrongly.
	const FMantleWheelItemMeasure Off = FMantleWheelItem::Measure(MakeRim(5.f, FVector3f(0.f, 30.f, 0.f)));
	TestFalse(TEXT("an off-axle rim is refused"), Off.bOk);
	TestTrue(TEXT("and told where"), Off.Problem.Contains(TEXT("origin")));
	TestFalse(TEXT("an off-center rim is refused"), FMantleWheelItem::Measure(MakeRim(5.f, FVector3f(30.f, 0.f, 0.f))).bOk);

	// A wheel on its side (long along X, small across).
	TArray<FVector3f> Side;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		Side.Add(FVector3f(Index * 10.f, 5.f * FMath::Cos(static_cast<float>(Index)), 5.f * FMath::Sin(static_cast<float>(Index))));
	}
	TestFalse(TEXT("a wheel on its side is refused"), FMantleWheelItem::Measure(Side).bOk);

	TestFalse(TEXT("too few points do not measure"), FMantleWheelItem::Measure(TArray<FVector3f>{ FVector3f::ZeroVector }).bOk);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
