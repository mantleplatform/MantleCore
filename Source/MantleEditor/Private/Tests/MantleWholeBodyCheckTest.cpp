// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanPose.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The whole-body check before the A-pose step scales the kept parts to full height, on synthetic bounds. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWholeBodyCheckTestPrivate
{
	// A 170 cm figure: head joint at 150 cm, ankles at 8 cm, 20 cm either side of the middle (span 142 cm).
	const FVector WholeBodyTestHead(0.0, 0.0, 150.0);
	const FVector WholeBodyTestLeftFoot(20.0, 0.0, 8.0);
	const FVector WholeBodyTestRightFoot(-20.0, 0.0, 8.0);

	FMantleWholeBodyCheck WholeBodyTestCheck(const FVector& Min, const FVector& Max)
	{
		return FMantleMetaHumanPose::CheckWholeBody(FBox3d(Min, Max), WholeBodyTestHead, WholeBodyTestLeftFoot, WholeBodyTestRightFoot);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWholeBodyCheckTest,
	"Mantle.Editor.MetaHumanPose.WholeBodyCheck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWholeBodyCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleWholeBodyCheckTestPrivate;

	// A whole body: floor to head top, wider than the feet.
	const FMantleWholeBodyCheck Whole = WholeBodyTestCheck(FVector(-35, -15, 0), FVector(35, 15, 170));
	TestTrue(TEXT("a whole body is checked"), Whole.bChecked);
	TestTrue(TEXT("a whole body passes"), Whole.Passes());
	TestEqual(TEXT("the joint span is head joint to the lower foot joint"), Whole.JointSpanCm, 142.0);
	TestEqual(TEXT("a whole body covers the span"), Whole.CoveredCm, 142.0);
	TestEqual(TEXT("nothing is missing"), Whole.Missing().Num(), 0);

	// Terra: only the head and neck kept (42 cm). Never scaled up.
	const FMantleWholeBodyCheck Head = WholeBodyTestCheck(FVector(-12, -12, 133), FVector(12, 12, 175));
	TestFalse(TEXT("a head alone fails"), Head.Passes());
	TestTrue(TEXT("a head alone reaches the head joint"), Head.bHead);
	TestFalse(TEXT("a head alone reaches no foot"), Head.bLeftFoot || Head.bRightFoot);
	TestFalse(TEXT("a head alone does not cover the span"), Head.bSpan);
	TestEqual(TEXT("a head alone misses the legs and feet"), FString::Join(Head.Missing(), TEXT("; ")), FString(TEXT("the legs and feet")));

	// Murdock: head and torso kept, no arms or legs.
	const FMantleWholeBodyCheck Torso = WholeBodyTestCheck(FVector(-30, -15, 90), FVector(30, 15, 175));
	TestFalse(TEXT("a head and torso fail"), Torso.Passes());
	TestTrue(TEXT("a head and torso reach the head joint"), Torso.bHead);

	// Twinblast with only its lower-body slots: the lower body only.
	const FMantleWholeBodyCheck Lower = WholeBodyTestCheck(FVector(-30, -15, 0), FVector(30, 15, 110));
	TestFalse(TEXT("a lower body alone fails"), Lower.Passes());
	TestTrue(TEXT("a lower body reaches both feet"), Lower.bLeftFoot && Lower.bRightFoot);
	TestEqual(TEXT("a lower body alone misses the head"), FString::Join(Lower.Missing(), TEXT("; ")), FString(TEXT("the head")));

	// One leg missing: the kept parts stop well short of the right foot.
	const FMantleWholeBodyCheck OneLeg = WholeBodyTestCheck(FVector(5, -15, 0), FVector(35, 15, 170));
	TestFalse(TEXT("one leg missing fails"), OneLeg.Passes());
	TestEqual(TEXT("one leg missing names it"), FString::Join(OneLeg.Missing(), TEXT("; ")), FString(TEXT("the right leg and foot")));

	// Within reach: the head top a little under the head joint still reaches it (10% of the span is 14.2 cm).
	TestTrue(TEXT("a joint within the reach counts"), WholeBodyTestCheck(FVector(-35, -15, 0), FVector(35, 15, 140)).bHead);
	TestFalse(TEXT("a joint beyond the reach does not"), WholeBodyTestCheck(FVector(-35, -15, 0), FVector(35, 15, 130)).bHead);

	// A bow taller than the body must fail the check.
	const FMantleWholeBodyCheck Bow = WholeBodyTestCheck(FVector(-35, -15, -20), FVector(35, 15, 230));
	TestFalse(TEXT("something much taller than the body fails"), Bow.Passes());
	TestFalse(TEXT("it is too tall"), Bow.bNotTooTall);
	TestTrue(TEXT("the message says so"), FMantleMetaHumanPose::WholeBodyMessage(Bow, TConstArrayView<FString>()).Contains(TEXT("taller than the body")));

	// The message: plain words, the missing parts, the heights, the slots to untick.
	const TArray<FString> Suggested = { TEXT("M03_Legs"), TEXT("M02_Arms") };
	const FString Message = FMantleMetaHumanPose::WholeBodyMessage(Torso, Suggested);
	TestTrue(TEXT("the message names the missing parts"), Message.Contains(TEXT("Missing: the legs and feet.")));
	TestTrue(TEXT("the message names the slots near them"), Message.Contains(TEXT("'M03_Legs', 'M02_Arms'")));
	TestTrue(TEXT("the message says what to do"), Message.Contains(TEXT("Under Strip, untick")));
	TestTrue(TEXT("the message gives the heights"), Message.Contains(TEXT("of the 142 cm")));

	// Nothing kept fails; a skeleton with no height can't be judged and is left to the later steps.
	TestFalse(TEXT("no kept parts fail"), FMantleMetaHumanPose::CheckWholeBody(FBox3d(ForceInit), WholeBodyTestHead, WholeBodyTestLeftFoot, WholeBodyTestRightFoot).Passes());
	const FMantleWholeBodyCheck Flat = FMantleMetaHumanPose::CheckWholeBody(FBox3d(FVector(-1), FVector(1)), FVector(0, 0, 5), FVector(0, 0, 10), FVector(0, 0, 10));
	TestFalse(TEXT("a head joint below the feet is not checked"), Flat.bChecked);
	TestTrue(TEXT("an unchecked body passes"), Flat.Passes());
	return true;
}

#endif
