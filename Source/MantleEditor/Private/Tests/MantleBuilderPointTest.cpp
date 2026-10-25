// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarHeight.h"
#include "MantleBuilderKind.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleBuilderPointTestPrivate
{
	static FMantleBuilderPoint MakePoint(FName Role, const FVector& Location, const FVector& GuessLocation, EMantleBuilderPointSource Source)
	{
		FMantleBuilderPoint P;
		P.Role = Role;
		P.Transform = FTransform(Location);
		P.Guess = FTransform(GuessLocation);
		P.Source = Source;
		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderMergeKeepsCreatorPointsTest, "Mantle.Editor.BuilderPoints.MergeKeepsCreatorPoints", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderMergeKeepsCreatorPointsTest::RunTest(const FString& Parameters)
{
	using namespace MantleBuilderPointTestPrivate;
	TArray<FMantleBuilderPoint> Existing;
	Existing.Add(MakePoint("A", FVector(1, 1, 1), FVector(2, 2, 2), EMantleBuilderPointSource::Creator));
	Existing.Add(MakePoint("B", FVector(3, 3, 3), FVector(3, 3, 3), EMantleBuilderPointSource::Guess));
	Existing.Add(MakePoint("Kept", FVector(9, 9, 9), FVector(9, 9, 9), EMantleBuilderPointSource::Creator));

	TArray<FMantleBuilderPoint> NewGuesses;
	NewGuesses.Add(MakePoint("A", FVector(10, 0, 0), FVector(10, 0, 0), EMantleBuilderPointSource::Guess));
	NewGuesses.Add(MakePoint("B", FVector(20, 0, 0), FVector(20, 0, 0), EMantleBuilderPointSource::Guess));
	NewGuesses.Add(MakePoint("New", FVector(30, 0, 0), FVector(30, 0, 0), EMantleBuilderPointSource::Guess));

	const TArray<FMantleBuilderPoint> Merged = MantleBuilderPoints::MergeGuesses(Existing, NewGuesses);
	TestEqual(TEXT("the old roles plus the new one"), Merged.Num(), 4);

	const FMantleBuilderPoint* A = MantleBuilderPoints::Find(Merged, "A");
	const FMantleBuilderPoint* B = MantleBuilderPoints::Find(Merged, "B");
	const FMantleBuilderPoint* New = MantleBuilderPoints::Find(Merged, "New");
	const FMantleBuilderPoint* Kept = MantleBuilderPoints::Find(Merged, "Kept");
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("New"), New) || !TestNotNull(TEXT("Kept"), Kept))
	{
		return false;
	}
	TestTrue(TEXT("a creator point keeps its transform"), A->Transform.GetLocation().Equals(FVector(1, 1, 1)));
	TestTrue(TEXT("a creator point gets the new guess"), A->Guess.GetLocation().Equals(FVector(10, 0, 0)));
	TestTrue(TEXT("a creator point stays creator"), A->Source == EMantleBuilderPointSource::Creator);
	TestTrue(TEXT("a guess point follows the new guess"), B->Transform.GetLocation().Equals(FVector(20, 0, 0)));
	TestTrue(TEXT("a new role is added at its guess"), New->Transform.GetLocation().Equals(FVector(30, 0, 0)) && New->Source == EMantleBuilderPointSource::Guess);
	TestTrue(TEXT("a role absent from the new guesses is unchanged"), Kept->Transform.GetLocation().Equals(FVector(9, 9, 9)) && Kept->Guess.GetLocation().Equals(FVector(9, 9, 9)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderResetTest, "Mantle.Editor.BuilderPoints.ResetToGuess", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderResetTest::RunTest(const FString& Parameters)
{
	using namespace MantleBuilderPointTestPrivate;
	FMantleBuilderPoint P = MakePoint("A", FVector(1, 1, 1), FVector(2, 2, 2), EMantleBuilderPointSource::Creator);
	P.ResetToGuess();
	TestTrue(TEXT("transform is the guess"), P.Transform.GetLocation().Equals(FVector(2, 2, 2)));
	TestTrue(TEXT("source is guess"), P.Source == EMantleBuilderPointSource::Guess);

	TArray<FMantleBuilderPoint> All;
	All.Add(MakePoint("A", FVector(1, 0, 0), FVector(5, 0, 0), EMantleBuilderPointSource::Creator));
	All.Add(MakePoint("B", FVector(2, 0, 0), FVector(6, 0, 0), EMantleBuilderPointSource::Creator));
	MantleBuilderPoints::ResetAll(All);
	for (const FMantleBuilderPoint& Point : All)
	{
		TestTrue(TEXT("every point is back at its guess"), Point.Transform.Equals(Point.Guess) && Point.Source == EMantleBuilderPointSource::Guess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderMirrorTest, "Mantle.Editor.BuilderPoints.MirrorRightHandToLeft", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderMirrorTest::RunTest(const FString& Parameters)
{
	FMantleBuilderPointRole HandR;
	HandR.Role = "Hand_R";
	HandR.MirrorRole = "Hand_L";
	FMantleBuilderPointRole Muzzle;
	Muzzle.Role = "Muzzle";
	TArray<FMantleBuilderPointRole> Roles;
	Roles.Add(HandR);
	Roles.Add(Muzzle);

	FMantleBuilderPoint From;
	From.Role = "Hand_R";
	From.Transform = FTransform(FRotator(0, 30, 0), FVector(10, 20, 5));

	FMantleBuilderPoint Out;
	if (!TestTrue(TEXT("Hand_R mirrors"), MantleBuilderPoints::MirrorPoint(From, Roles, Out)))
	{
		return false;
	}
	TestEqual(TEXT("role"), Out.Role, FName("Hand_L"));
	TestTrue(TEXT("location y is negated"), Out.Transform.GetLocation().Equals(FVector(10, -20, 5), 0.001));
	TestEqual(TEXT("yaw is mirrored"), Out.Transform.Rotator().Yaw, -30.0, 0.001);
	TestTrue(TEXT("source is creator"), Out.Source == EMantleBuilderPointSource::Creator);

	FMantleBuilderPoint Plain;
	Plain.Role = "Muzzle";
	TestFalse(TEXT("a role with no mirror role"), MantleBuilderPoints::MirrorPoint(Plain, Roles, Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderSolveLimbTest, "Mantle.Editor.BuilderPoints.SolveLimbReaches", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderSolveLimbTest::RunTest(const FString& Parameters)
{
	FVector Mid, End;
	bool bReached = false;
	FMantleBodyMeasurements::SolveLimb(FVector::ZeroVector, FVector(30, 0, 0), 20.f, 20.f, FVector::UpVector, Mid, End, bReached);
	TestTrue(TEXT("reached"), bReached);
	TestTrue(TEXT("mid is above the line"), Mid.Z > 0.0);
	TestEqual(TEXT("upper length"), Mid.Size(), 20.0, 0.01);
	TestEqual(TEXT("lower length"), (FVector(30, 0, 0) - Mid).Size(), 20.0, 0.01);
	TestTrue(TEXT("end on target"), End.Equals(FVector(30, 0, 0), 0.01));

	FMantleBodyMeasurements::SolveLimb(FVector::ZeroVector, FVector(50, 0, 0), 20.f, 20.f, FVector::UpVector, Mid, End, bReached);
	TestFalse(TEXT("out of reach"), bReached);
	TestTrue(TEXT("end clamped at full reach"), End.Equals(FVector(40, 0, 0), 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderBodyMeasurementsTest, "Mantle.Editor.BuilderPoints.BodyMeasurements", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderBodyMeasurementsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("height is the avatar target height"), double(FMantleBodyMeasurements::HeightCm), FMantleAvatarHeight::TargetCm(), 0.01);

	double HighestTop = 0.0;
	for (const FMantleGhostCapsule& C : FMantleBodyMeasurements::StandingGhost())
	{
		TestTrue(*FString::Printf(TEXT("%s has a radius"), *C.Part.ToString()), C.RadiusCm > 0.f);
		HighestTop = FMath::Max(HighestTop, FMath::Max(C.Start.Z, C.End.Z) + double(C.RadiusCm));
	}
	TestEqual(TEXT("the top of the head is the height"), HighestTop, double(FMantleBodyMeasurements::HeightCm), FMantleBodyMeasurements::HeightCm * 0.05);

	for (const FMantleGhostCapsule& C : FMantleBodyMeasurements::SeatedGhost())
	{
		TestTrue(*FString::Printf(TEXT("seated %s has a radius"), *C.Part.ToString()), C.RadiusCm > 0.f);
	}
	return true;
}

#endif
