// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMetaHumanPose.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The secant realignment after Epic's chain-to-chain auto-align, pure maths on synthetic chains. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSecantAlignTestPrivate
{
	/** The angle between two directions, degrees. */
	double SecantTestDegrees(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}

	/** MetaHuman-like source: a 5-joint S-curved spine, a 2-joint neck and the head, component space (cm). */
	const TArray<FVector>& SecantTestSourceSpine()
	{
		static const TArray<FVector> Spine = { FVector(0, 0, 105), FVector(2, 0, 112), FVector(3, 0, 122), FVector(1, 0, 135), FVector(-2, 0, 150) };
		return Spine;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecantAlignShortSpineTest,
	"Mantle.Editor.MetaHumanPose.SecantAlign.ShortSpineAndOneBoneNeck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecantAlignShortSpineTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecantAlignTestPrivate;
	// pelvis 0, spine_01..03 1-3, neck_01 4, head 5, clavicle 6 (under spine_03, off the chain's line); a straight spine
	// leaning forward, as a 3-bone spine of the UE4 layout.
	const TArray<int32> Parents = { INDEX_NONE, 0, 1, 2, 3, 4, 3 };
	const FVector Lean = FVector(0.3, 0.0, 1.0).GetSafeNormal();
	const FVector Base(0, 0, 105);
	TArray<FVector> Joints = { FVector(0, 0, 100), Base, Base + Lean * 15.0, Base + Lean * 35.0, Base + Lean * 55.0, Base + Lean * 65.0, Base + Lean * 35.0 + FVector(0, 15, 0) };
	const TArray<FVector> Before = Joints;

	TArray<FMantleSecantChain> Chains;
	FMantleSecantChain& Spine = Chains.AddDefaulted_GetRef();
	Spine.Name = FName(TEXT("Spine"));
	Spine.Bones = { 1, 2, 3 };
	Spine.Source = SecantTestSourceSpine();
	FMantleSecantChain& Neck = Chains.AddDefaulted_GetRef();
	Neck.Name = FName(TEXT("Neck"));
	Neck.Bones = { 4 };
	Neck.Source = { FVector(-3, 0, 158), FVector(-3, 0, 164) };
	FMantleSecantChain& Head = Chains.AddDefaulted_GetRef();
	Head.Name = FName(TEXT("Head"));
	Head.Bones = { 5 };
	Head.Source = { FVector(-2, 0, 170) };

	const TArray<FVector>& Source = SecantTestSourceSpine();
	const FVector SourceChord = Source.Last() - Source[0];
	const double ChordBefore = SecantTestDegrees(Joints[3] - Joints[1], SourceChord);
	const TArray<FMantleSecantTurn> Turns = FMantleMetaHumanPose::SecantAlign(Parents, Joints, Chains, /*SkipBone*/ 0);
	const double ChordAfter = SecantTestDegrees(Joints[3] - Joints[1], SourceChord);
	AddInfo(FString::Printf(TEXT("spine chord %.3f -> %.3f degrees, %d turns"), ChordBefore, ChordAfter, Turns.Num()));
	TestTrue(TEXT("the leaning straight spine starts well off the S-curve's chord"), ChordBefore > 10.0);
	TestTrue(TEXT("the 3-joint spine's chord is within 0.5 degrees of the 5-joint S-curve's"), ChordAfter < 0.5);

	// One-bone neck: aimed from its joint to the head's, as the source runs from its neck's start to its head's.
	const double NeckAfter = SecantTestDegrees(Joints[5] - Joints[4], Head.Source[0] - Neck.Source[0]);
	AddInfo(FString::Printf(TEXT("one-bone neck to head %.4f degrees from the source's"), NeckAfter));
	TestTrue(TEXT("the one-bone neck aims at the next chain's start"), NeckAfter < 0.01);

	// Bone lengths kept; the pelvis (skipped) and the spine's start untouched; the head (no next chain) and the last spine
	// bone are never turned themselves.
	for (int32 Bone = 1; Bone < Parents.Num(); ++Bone)
	{
		const double LengthBefore = FVector::Dist(Before[Bone], Before[Parents[Bone]]);
		const double LengthAfter = FVector::Dist(Joints[Bone], Joints[Parents[Bone]]);
		TestTrue(FString::Printf(TEXT("bone %d keeps its length (%.4f, %.4f)"), Bone, LengthBefore, LengthAfter), FMath::Abs(LengthBefore - LengthAfter) < 1e-3);
	}
	TestTrue(TEXT("the pelvis and the spine's start stay put"), Joints[0].Equals(Before[0], 1e-6) && Joints[1].Equals(Before[1], 1e-6));
	for (const FMantleSecantTurn& Turn : Turns)
	{
		TestTrue(FString::Printf(TEXT("bone %d may turn (spine_01, spine_02, neck_01 only)"), Turn.Bone), Turn.Bone == 1 || Turn.Bone == 2 || Turn.Bone == 4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecantAlignFiveJointTest,
	"Mantle.Editor.MetaHumanPose.SecantAlign.FiveJointSpineStays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecantAlignFiveJointTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecantAlignTestPrivate;
	// A 5-joint spine shaped as the source, 10% smaller and moved: every bone already lies along its stretch of source.
	const TArray<FVector>& Source = SecantTestSourceSpine();
	const TArray<int32> Parents = { INDEX_NONE, 0, 1, 2, 3, 4 };
	TArray<FVector> Joints = { FVector(0, 0, 90) };
	for (const FVector& Joint : Source)
	{
		Joints.Add(FVector(5, 5, 0) + Joint * 0.9);
	}
	const TArray<FVector> Before = Joints;
	TArray<FMantleSecantChain> Chains;
	FMantleSecantChain& Spine = Chains.AddDefaulted_GetRef();
	Spine.Name = FName(TEXT("Spine"));
	Spine.Bones = { 1, 2, 3, 4, 5 };
	Spine.Source = Source;
	const TArray<FMantleSecantTurn> Turns = FMantleMetaHumanPose::SecantAlign(Parents, Joints, Chains, /*SkipBone*/ 0);
	double Largest = 0.0;
	for (const FMantleSecantTurn& Turn : Turns)
	{
		Largest = FMath::Max(Largest, FMath::RadiansToDegrees(Turn.Rotation.GetAngle()));
	}
	const double Chord = SecantTestDegrees(Joints[5] - Joints[1], Source.Last() - Source[0]);
	AddInfo(FString::Printf(TEXT("5-joint spine: %d turns, largest %.6f degrees, chord %.6f degrees"), Turns.Num(), Largest, Chord));
	TestTrue(TEXT("no bone of a 5-joint spine shaped as the source turns"), Largest < 0.01);
	TestTrue(TEXT("the 5-joint spine's chord stays at 0"), Chord < 0.01);
	for (int32 Bone = 0; Bone < Joints.Num(); ++Bone)
	{
		TestTrue(FString::Printf(TEXT("joint %d stays put"), Bone), Joints[Bone].Equals(Before[Bone], 1e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecantAlignLeftAloneTest,
	"Mantle.Editor.MetaHumanPose.SecantAlign.LeavesUnmappedAndLastBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecantAlignLeftAloneTest::RunTest(const FString& Parameters)
{
	// A one-bone Head (no next chain), a one-bone Neck whose next chain has no source, and a chain with no source: nothing turns.
	const TArray<int32> Parents = { INDEX_NONE, 0, 1, 2 };
	TArray<FVector> Joints = { FVector(0, 0, 100), FVector(1, 0, 110), FVector(3, 0, 120), FVector(4, 0, 130) };
	TArray<FMantleSecantChain> Chains;
	FMantleSecantChain& Neck = Chains.AddDefaulted_GetRef();
	Neck.Name = FName(TEXT("Neck"));
	Neck.Bones = { 1 };
	Neck.Source = { FVector(0, 0, 150) };
	FMantleSecantChain& Head = Chains.AddDefaulted_GetRef();
	Head.Name = FName(TEXT("Head"));
	Head.Bones = { 2 };
	FMantleSecantChain& Tail = Chains.AddDefaulted_GetRef();
	Tail.Name = FName(TEXT("Tail"));
	Tail.Bones = { 3 };
	Tail.Source = { FVector(0, 0, 0) };
	const TArray<FVector> Before = Joints;
	const TArray<FMantleSecantTurn> Turns = FMantleMetaHumanPose::SecantAlign(Parents, Joints, Chains, /*SkipBone*/ 0);
	TestEqual(TEXT("nothing turns"), Turns.Num(), 0);
	for (int32 Bone = 0; Bone < Joints.Num(); ++Bone)
	{
		TestTrue(FString::Printf(TEXT("joint %d stays put"), Bone), Joints[Bone].Equals(Before[Bone], 1e-9));
	}
	return true;
}

#endif
