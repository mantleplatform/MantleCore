// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarReposeFixer.h"
#include "MantleAvatarRetargetPoseFixer.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// An As-is body's finger bones take the target skeleton's axes, so the client's FK finger chains twist nothing. Synthetic
// skeleton and template, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleFingerAxesTestPrivate
{
	/** A small humanoid (pelvis, both arms with five fingers each, both legs) and the retargeter's side of it. */
	struct FFingerAxesFixture
	{
		TArray<FName> Names;
		TArray<int32> Parents;
		TArray<FTransform> Template;
		FMantleRetargetPoseDriver Driver;

		int32 Add(const TCHAR* Name, const int32 Parent, const FVector& Location)
		{
			Names.Add(FName(Name));
			Parents.Add(Parent);
			Template.Add(FTransform(FQuat::Identity, Location));
			return Names.Num() - 1;
		}

		int32 Find(const TCHAR* Name) const { return Names.IndexOfByKey(FName(Name)); }
	};

	/** Adds one hand's three-bone fingers; the thumb leans out to the front, the others run straight out. */
	void FingerAxesAddHand(FFingerAxesFixture& Fixture, const int32 Hand, const FVector& HandLocation, const double Side, const TCHAR* Suffix)
	{
		struct FFinger { const TCHAR* Name; FVector Offset; FVector Direction; };
		const FFinger Fingers[] = {
			{ TEXT("thumb"), FVector(Side * 3.0, -3.0, -1.0), FVector(Side * 0.7, -0.7, 0.0) },
			{ TEXT("index"), FVector(Side * 8.0, -2.0, 0.0), FVector(Side, 0.0, 0.0) },
			{ TEXT("middle"), FVector(Side * 8.0, 0.0, 0.0), FVector(Side, 0.0, 0.0) },
			{ TEXT("ring"), FVector(Side * 8.0, 2.0, 0.0), FVector(Side, 0.0, 0.0) },
			{ TEXT("pinky"), FVector(Side * 7.0, 4.0, 0.0), FVector(Side, 0.0, 0.0) },
		};
		const double Lengths[] = { 4.0, 3.0, 2.0 };
		for (const FFinger& Finger : Fingers)
		{
			FVector Location = HandLocation + Finger.Offset;
			int32 Parent = Hand;
			for (int32 Segment = 1; Segment <= 3; ++Segment)
			{
				Parent = Fixture.Add(*FString::Printf(TEXT("%s_%02d_%s"), Finger.Name, Segment, Suffix), Parent, Location);
				Location += Finger.Direction.GetSafeNormal() * Lengths[Segment - 1];
			}
		}
	}

	/**
	 * The template: every bone's X axis along its first child (a tip takes its parent's), each rolled by its own angle, so the
	 * axes are not the identity anywhere.
	 */
	FFingerAxesFixture FingerAxesMakeFixture()
	{
		FFingerAxesFixture Fixture;
		const int32 Pelvis = Fixture.Add(TEXT("pelvis"), INDEX_NONE, FVector(0, 0, 100));
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const double Sign = Side == 0 ? 1.0 : -1.0;
			const TCHAR* Suffix = Side == 0 ? TEXT("l") : TEXT("r");
			const auto Named = [Suffix](const TCHAR* Bone) { return FString::Printf(TEXT("%s_%s"), Bone, Suffix); };
			const int32 Upper = Fixture.Add(*Named(TEXT("upperarm")), Pelvis, FVector(Sign * 20, 0, 150));
			const int32 Lower = Fixture.Add(*Named(TEXT("lowerarm")), Upper, FVector(Sign * 45, 0, 150));
			const int32 Hand = Fixture.Add(*Named(TEXT("hand")), Lower, FVector(Sign * 70, 0, 150));
			FingerAxesAddHand(Fixture, Hand, FVector(Sign * 70, 0, 150), Sign, Suffix);
			const int32 Thigh = Fixture.Add(*Named(TEXT("thigh")), Pelvis, FVector(Sign * 10, 0, 95));
			const int32 Calf = Fixture.Add(*Named(TEXT("calf")), Thigh, FVector(Sign * 10, 0, 50));
			const int32 Foot = Fixture.Add(*Named(TEXT("foot")), Calf, FVector(Sign * 10, 0, 8));
			Fixture.Add(*Named(TEXT("ball")), Foot, FVector(Sign * 10, 12, 0));
		}

		for (int32 Bone = 0; Bone < Fixture.Names.Num(); ++Bone)
		{
			const int32 Child = Fixture.Parents.IndexOfByKey(Bone);
			FQuat Rotation;
			if (Child != INDEX_NONE)
			{
				const FVector Direction = (Fixture.Template[Child].GetLocation() - Fixture.Template[Bone].GetLocation()).GetSafeNormal();
				Rotation = FQuat(Direction, FMath::DegreesToRadians(17.0 * (Bone % 4))) * FQuat::FindBetweenVectors(FVector::XAxisVector, Direction);
			}
			else
			{
				Rotation = Fixture.Template[Fixture.Parents[Bone]].GetRotation();
			}
			Fixture.Template[Bone].SetRotation(Rotation.GetNormalized());
		}
		for (int32 Bone = 0; Bone < Fixture.Names.Num(); ++Bone)
		{
			Fixture.Driver.DriverJoints.Add(Fixture.Names[Bone], Fixture.Template[Bone].GetLocation());
			Fixture.Driver.TargetReference.Add(Fixture.Names[Bone], Fixture.Template[Bone]);
		}
		return Fixture;
	}

	/** A component-space pose as the bone-space pose the plan reads. */
	TArray<FTransform> FingerAxesLocal(const FFingerAxesFixture& Fixture, const TArray<FTransform>& Component)
	{
		TArray<FTransform> Local;
		for (int32 Bone = 0; Bone < Component.Num(); ++Bone)
		{
			const int32 Parent = Fixture.Parents[Bone];
			Local.Add(Parent != INDEX_NONE ? Component[Bone].GetRelativeTransform(Component[Parent]) : Component[Bone]);
		}
		return Local;
	}

	bool FingerAxesIsFinger(const FName Bone)
	{
		for (const TArray<FName>& Chain : FMantleAvatarRetargetPose::FingerChains())
		{
			if (Chain.Contains(Bone))
			{
				return true;
			}
		}
		return false;
	}

	/** The bone's axis the template points along its segment (X). */
	FVector FingerAxesPointing(const FTransform& Bone) { return Bone.GetRotation().RotateVector(FVector::XAxisVector); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFingerAxesRolledAndFlippedTest,
	"Mantle.Editor.AvatarRetargetPose.FingerAxes.RolledAndFlippedFingersTakeTheTargetAxes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFingerAxesRolledAndFlippedTest::RunTest(const FString& Parameters)
{
	using namespace MantleFingerAxesTestPrivate;
	const FFingerAxesFixture Fixture = FingerAxesMakeFixture();

	// The creator's fingers: the same joints, every finger bone rolled a quarter turn about its own length, and the
	// index_02_l bone also flipped (its X axis pointing back along the finger).
	TArray<FTransform> Body = Fixture.Template;
	int32 NumFingerBones = 0;
	for (int32 Bone = 0; Bone < Body.Num(); ++Bone)
	{
		if (!FingerAxesIsFinger(Fixture.Names[Bone]))
		{
			continue;
		}
		++NumFingerBones;
		const FQuat Template = Fixture.Template[Bone].GetRotation();
		FQuat Rolled = FQuat(FingerAxesPointing(Fixture.Template[Bone]), FMath::DegreesToRadians(90.0)) * Template;
		if (Fixture.Names[Bone] == FName(TEXT("index_02_l")))
		{
			Rolled = FQuat(Template.GetUpVector(), PI) * Template;
		}
		Body[Bone].SetRotation(Rolled.GetNormalized());
	}
	TestEqual(TEXT("30 finger bones in the fixture"), NumFingerBones, 30);

	const TArray<FTransform> Local = FingerAxesLocal(Fixture, Body);
	FMantleRetargetPosePlan Plan;
	FString Error;
	if (!TestTrue(TEXT("plans"), FMantleAvatarRetargetPose::Plan(Fixture.Names, Fixture.Parents, Local, Fixture.Driver, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("every finger bone is realigned"), Plan.FingerReframes.Num(), NumFingerBones);
	TestEqual(TEXT("no limb needed new axes"), Plan.Reframes.Num(), 0);
	TestEqual(TEXT("no limb turned"), Plan.Turns.Num(), 0);
	TestFalse(TEXT("the plan is not empty"), Plan.IsEmpty());
	TestTrue(TEXT("the flipped bone is the largest turn (180 degrees)"), FMath::IsNearlyEqual(Plan.MaxFingerReframeDegrees(), 180.0, 0.01));

	// Every finger bone now has the template's axes; the rest of the body is as it was.
	TArray<FTransform> After;
	FMantleAvatarRepose::ComposeComponent(Fixture.Parents, Plan.LocalPose, After);
	for (int32 Bone = 0; Bone < After.Num(); ++Bone)
	{
		const FTransform& Want = FingerAxesIsFinger(Fixture.Names[Bone]) ? Fixture.Template[Bone] : Body[Bone];
		TestTrue(*FString::Printf(TEXT("%s has the wanted axes"), *Fixture.Names[Bone].ToString()),
			FMath::RadiansToDegrees(After[Bone].GetRotation().AngularDistance(Want.GetRotation())) < 0.01);
		TestTrue(*FString::Printf(TEXT("%s has not moved"), *Fixture.Names[Bone].ToString()), After[Bone].GetLocation().Equals(Body[Bone].GetLocation(), 1.e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFingerAxesMatchingTest,
	"Mantle.Editor.AvatarRetargetPose.FingerAxes.AMatchingBodyIsLeftAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFingerAxesMatchingTest::RunTest(const FString& Parameters)
{
	using namespace MantleFingerAxesTestPrivate;
	const FFingerAxesFixture Fixture = FingerAxesMakeFixture();
	const TArray<FTransform> Local = FingerAxesLocal(Fixture, Fixture.Template);
	FMantleRetargetPosePlan Plan;
	FString Error;
	if (!TestTrue(TEXT("plans"), FMantleAvatarRetargetPose::Plan(Fixture.Names, Fixture.Parents, Local, Fixture.Driver, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("no finger realigned"), Plan.FingerReframes.Num(), 0);
	TestTrue(TEXT("nothing to do"), Plan.IsEmpty());
	TestEqual(TEXT("largest finger turn"), Plan.MaxFingerReframeDegrees(), 0.0);
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		TestTrue(*FString::Printf(TEXT("%s is unchanged"), *Fixture.Names[Bone].ToString()),
			Plan.LocalPose[Bone].GetRotation().AngularDistance(Local[Bone].GetRotation()) < 1.e-6 && Plan.LocalPose[Bone].GetLocation().Equals(Local[Bone].GetLocation(), 1.e-6));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFingerAxesCurledFingerTest,
	"Mantle.Editor.AvatarRetargetPose.FingerAxes.AFingerOffTheTemplateKeepsItsOwnDirectionAndTheMeshStays",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFingerAxesCurledFingerTest::RunTest(const FString& Parameters)
{
	using namespace MantleFingerAxesTestPrivate;
	const FFingerAxesFixture Fixture = FingerAxesMakeFixture();

	// The creator's left index finger is bent 30 degrees sideways from the first joint on; its bones keep the template's
	// axes (the mesh is not rolled, only curled), so what must change is the swing onto the finger's own segments.
	TArray<FTransform> Body = Fixture.Template;
	const int32 First = Fixture.Find(TEXT("index_01_l"));
	const int32 Second = Fixture.Find(TEXT("index_02_l"));
	const int32 Tip = Fixture.Find(TEXT("index_03_l"));
	const FQuat Curl(FVector::ZAxisVector, FMath::DegreesToRadians(30.0));
	for (const int32 Bone : { Second, Tip })
	{
		Body[Bone].SetLocation(Body[First].GetLocation() + Curl.RotateVector(Fixture.Template[Bone].GetLocation() - Body[First].GetLocation()));
	}

	const TArray<FTransform> Local = FingerAxesLocal(Fixture, Body);
	FMantleRetargetPosePlan Plan;
	FString Error;
	if (!TestTrue(TEXT("plans"), FMantleAvatarRetargetPose::Plan(Fixture.Names, Fixture.Parents, Local, Fixture.Driver, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the three bones of that finger are realigned"), Plan.FingerReframes.Num(), 3);
	TestTrue(TEXT("by 30 degrees"), FMath::IsNearlyEqual(Plan.MaxFingerReframeDegrees(), 30.0, 0.01));

	TArray<FTransform> After;
	FMantleAvatarRepose::ComposeComponent(Fixture.Parents, Plan.LocalPose, After);
	// The first two bones point along the creator's segments.
	const FVector FirstSegment = (Body[Second].GetLocation() - Body[First].GetLocation()).GetSafeNormal();
	const FVector SecondSegment = (Body[Tip].GetLocation() - Body[Second].GetLocation()).GetSafeNormal();
	TestTrue(TEXT("index_01_l points along its segment"), FingerAxesPointing(After[First]).Equals(FirstSegment, 1.e-4));
	TestTrue(TEXT("index_02_l points along its segment"), FingerAxesPointing(After[Second]).Equals(SecondSegment, 1.e-4));
	// Their roll about it is the template's, carried along (the template's own angle between them kept).
	const FQuat TemplateRelative = Fixture.Template[First].GetRotation().Inverse() * Fixture.Template[Second].GetRotation();
	const FQuat ResultRelative = After[First].GetRotation().Inverse() * After[Second].GetRotation();
	TestTrue(TEXT("the bones keep the template's roll between them"), TemplateRelative.AngularDistance(ResultRelative) < 1.e-4);

	// The mesh does not move: no joint moves, no turn is made, and so the skinning matrix of every bone (old bind to new, as
	// FMantleAvatarRepose::RebindMeshDescription builds it from the plan's FramedPose and LocalPose) is the identity.
	TestEqual(TEXT("no step 2 turn"), Plan.Turns.Num(), 0);
	TArray<FTransform> Framed;
	FMantleAvatarRepose::ComposeComponent(Fixture.Parents, Plan.FramedPose, Framed);
	for (int32 Bone = 0; Bone < After.Num(); ++Bone)
	{
		TestTrue(*FString::Printf(TEXT("%s has not moved"), *Fixture.Names[Bone].ToString()), After[Bone].GetLocation().Equals(Body[Bone].GetLocation(), 1.e-3));
		const FMatrix Skin = Framed[Bone].ToMatrixWithScale().Inverse() * After[Bone].ToMatrixWithScale();
		TestTrue(*FString::Printf(TEXT("%s skins to the identity"), *Fixture.Names[Bone].ToString()), Skin.Equals(FMatrix::Identity, 1.e-4));
		// A vertex on the finger, in the creator's bind, lands on itself in the new bind.
		const FVector Vertex = Body[Bone].GetLocation() + FVector(0.3, 0.2, 0.1);
		TestTrue(*FString::Printf(TEXT("a vertex of %s stays"), *Fixture.Names[Bone].ToString()), FVector(Skin.TransformPosition(Vertex)).Equals(Vertex, 1.e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFingerAxesNoFingerTemplateTest,
	"Mantle.Editor.AvatarRetargetPose.FingerAxes.ATargetSkeletonWithoutFingersLeavesThemAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFingerAxesNoFingerTemplateTest::RunTest(const FString& Parameters)
{
	using namespace MantleFingerAxesTestPrivate;
	FFingerAxesFixture Fixture = FingerAxesMakeFixture();
	TArray<FTransform> Body = Fixture.Template;
	for (int32 Bone = 0; Bone < Body.Num(); ++Bone)
	{
		if (FingerAxesIsFinger(Fixture.Names[Bone]))
		{
			Body[Bone].SetRotation((FQuat(FVector::XAxisVector, FMath::DegreesToRadians(90.0)) * Body[Bone].GetRotation()).GetNormalized());
			Fixture.Driver.TargetReference.Remove(Fixture.Names[Bone]);
		}
	}
	const TArray<FTransform> Local = FingerAxesLocal(Fixture, Body);
	FMantleRetargetPosePlan Plan;
	FString Error;
	if (!TestTrue(TEXT("plans"), FMantleAvatarRetargetPose::Plan(Fixture.Names, Fixture.Parents, Local, Fixture.Driver, Plan, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("no finger realigned"), Plan.FingerReframes.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
