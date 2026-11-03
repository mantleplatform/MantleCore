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
	TestEqual(TEXT("the face is toward +X when the hub sits toward +X"), Good.FaceSign, 1);
	TestEqual(TEXT("a centered rim has nothing to fix"), Good.Notes.Num(), 0);

	const FMantleWheelItemMeasure Back = FMantleWheelItem::Measure(MakeRim(-5.f));
	TestEqual(TEXT("the face is toward -X when the hub sits toward -X"), Back.FaceSign, -1);
	TestEqual(TEXT("and the creator is told"), Back.Notes.Num(), 1);

	const FMantleWheelItemMeasure Flat = FMantleWheelItem::Measure(MakeRim(0.f));
	TestEqual(TEXT("a hub in the middle cannot be read"), Flat.FaceSign, 0);

	const FMantleWheelItemMeasure Off = FMantleWheelItem::Measure(MakeRim(5.f, FVector3f(0.f, 30.f, 0.f)));
	TestTrue(TEXT("an off-axle rim still measures, with a note"), Off.bOk && Off.Notes.Num() >= 1);

	TestFalse(TEXT("too few points do not measure"), FMantleWheelItem::Measure(TArray<FVector3f>{ FVector3f::ZeroVector }).bOk);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
