// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderKind.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The blade holds draw a closed fist round the handle, not the gun-grip hand.

namespace MantleWeaponBuilderFistTestPrivate
{
	static FMantleBuilderPoint MakePoint(const TCHAR* Role, const FVector& Location)
	{
		FMantleBuilderPoint Point;
		Point.Role = FName(Role);
		Point.Transform = FTransform(Location);
		return Point;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderFistTest,
	"Mantle.Editor.WeaponBuilder.FistGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderFistTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderFistTestPrivate;
	const FVector Palm(10.0, 0.0, 0.0);
	const FVector Handle = Palm + FVector(0.0, 0.0, 2.0);

	const FMantleWeaponBuilderKind Sword(EMantleWeaponFitHold::Melee1h);
	const TArray<FMantleGhostCapsule> Fist = Sword.GhostPose({ MakePoint(TEXT("Grip_R"), Palm) });
	int32 Fingers = 0;
	bool bBothSides = false;
	bool bBelow = false;
	bool bAbove = false;
	for (const FMantleGhostCapsule& Capsule : Fist)
	{
		if (!Capsule.Part.ToString().StartsWith(TEXT("Finger_")))
		{
			continue;
		}
		++Fingers;
		for (const FVector& End : { Capsule.Start, Capsule.End })
		{
			// Distance from the handle's centre line (parallel to X) in the Y-Z plane.
			const double Off = FVector2D(End.Y - Handle.Y, End.Z - Handle.Z).Size();
			TestTrue(*FString::Printf(TEXT("%s stays within 3.5 cm of the handle (%.2f)"), *Capsule.Part.ToString(), Off), Off <= 3.5);
			bBothSides |= End.Y > Handle.Y + 0.5;
			bBelow |= End.Y < Handle.Y - 0.5;
			bAbove |= End.Z > Handle.Z + 0.5;
		}
	}
	TestEqual(TEXT("four fingers of three capsules"), Fingers, 12);
	TestTrue(TEXT("the fingers are on both sides of the handle"), bBothSides && bBelow);
	TestTrue(TEXT("the fingers pass over the handle's top"), bAbove);

	const TArray<FMantleGhostCapsule> Two = FMantleWeaponBuilderKind(EMantleWeaponFitHold::Melee2h).GhostPose({ MakePoint(TEXT("Grip_R"), Palm), MakePoint(TEXT("Grip_L"), Palm + FVector(-12.0, 0.0, 0.0)) });
	const auto CountFingers = [](const TArray<FMantleGhostCapsule>& Ghost)
	{
		return Ghost.FilterByPredicate([](const FMantleGhostCapsule& C) { return C.Part.ToString().StartsWith(TEXT("Finger_")); }).Num();
	};
	TestEqual(TEXT("a two-handed blade draws two fists"), CountFingers(Two), Fingers * 2);
	TestTrue(TEXT("the body holding it is drawn too"), Two.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Torso")); }));

	// Guns keep the gun hand.
	const FMantleWeaponBuilderKind Pistol(EMantleWeaponFitHold::Pistol);
	const FMantleBuilderPoint Muzzle = MakePoint(TEXT("Muzzle"), FVector(20.0, 0.0, 5.0));
	const FMantleBuilderPoint Sight = MakePoint(TEXT("Sight"), FVector(10.0, 0.0, 8.0));
	const TArray<FMantleGhostCapsule> Gun = Pistol.GhostPose({ MakePoint(TEXT("Grip_R"), Palm), Muzzle, Sight });
	TestTrue(TEXT("the pistol ghost still draws its arm and hand"), Gun.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Hand_R")); }));
	TestTrue(TEXT("the pistol ghost keeps its capsule count"), Gun.Num() > 0);
	TestFalse(TEXT("the pistol has no fist wrist"), Gun.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Wrist")); }));
	return true;
}
// The body holding a held weapon: it stands upright in the builder's view, every arm reaches for the usual hand spacings,
// and each forearm ends on its hand's wrist (no gap between the arm and the hand).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderHeldBodyTest,
	"Mantle.Editor.WeaponBuilder.HeldBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderHeldBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderFistTestPrivate;
	struct FCase
	{
		const TCHAR* What;
		EMantleWeaponFitHold Hold;
		TArray<FMantleBuilderPoint> Points;
		FVector ViewUpInWeapon;
	};
	const FVector Grip(0.0, 0.0, -2.0);
	const TArray<FCase> Cases = {
		{ TEXT("one-handed sword"), EMantleWeaponFitHold::Melee1h, { MakePoint(TEXT("Grip_R"), Grip) }, FVector::ForwardVector },
		{ TEXT("greatsword"), EMantleWeaponFitHold::Melee2h, { MakePoint(TEXT("Grip_R"), Grip), MakePoint(TEXT("Grip_L"), Grip + FVector(12.0, 0.0, 0.0)) }, FVector::ForwardVector },
		{ TEXT("spear"), EMantleWeaponFitHold::Spear, { MakePoint(TEXT("Grip_R"), Grip), MakePoint(TEXT("Grip_L"), Grip + FVector(32.0, 0.0, 0.0)) }, FVector::ForwardVector },
		{ TEXT("staff"), EMantleWeaponFitHold::Staff, { MakePoint(TEXT("Grip_R"), Grip), MakePoint(TEXT("Grip_L"), Grip + FVector(-30.0, 0.0, 0.0)) }, FVector::ForwardVector },
		{ TEXT("knife"), EMantleWeaponFitHold::Throwable, { MakePoint(TEXT("Grip_R"), Grip) }, FVector::ForwardVector },
		{ TEXT("bow"), EMantleWeaponFitHold::Bow, { MakePoint(TEXT("Grip_L"), FVector::ZeroVector), MakePoint(TEXT("Nock"), FVector(-51.0, 17.0, 16.0)) }, FVector::UpVector },
		{ TEXT("shield"), EMantleWeaponFitHold::Shield, { MakePoint(TEXT("Grip"), FVector::ZeroVector) }, FVector::UpVector },
	};
	for (const FCase& Case : Cases)
	{
		const FMantleWeaponBuilderKind Kind(Case.Hold);
		const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Case.Points);
		const FMantleGhostCapsule* Head = Ghost.FindByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Head")); });
		const FMantleGhostCapsule* Torso = Ghost.FindByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Torso")); });
		if (!TestNotNull(*FString::Printf(TEXT("%s: a body"), Case.What), Head) || !TestNotNull(*FString::Printf(TEXT("%s: a torso"), Case.What), Torso))
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s: every arm reaches"), Case.What), Ghost.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.bWarning; }));
		// The view turns the weapon's up for this hold onto the view's up, and the head is above the torso there.
		const FQuat Turn = Kind.ViewTurn();
		TestTrue(*FString::Printf(TEXT("%s: the holder's up is the view's up"), Case.What), Turn.RotateVector(Case.ViewUpInWeapon).Equals(FVector::UpVector, 1.0e-4));
		const double HeadZ = Turn.RotateVector((Head->Start + Head->End) * 0.5).Z;
		const double TorsoZ = Turn.RotateVector((Torso->Start + Torso->End) * 0.5).Z;
		TestTrue(*FString::Printf(TEXT("%s: the body stands (head %.0f over torso %.0f)"), Case.What, HeadZ, TorsoZ), HeadZ > TorsoZ + 20.0);
		// Each forearm ends where its hand begins: on the fist's wrist, or the back of a gun-style hand.
		for (const TCHAR* Side : { TEXT("R"), TEXT("L") })
		{
			const FMantleGhostCapsule* Forearm = Ghost.FindByPredicate([Side](const FMantleGhostCapsule& C) { return C.Part == FName(FString::Printf(TEXT("LowerArm_%s"), Side)); });
			if (!Forearm)
			{
				continue;
			}
			const bool bJoined = Ghost.ContainsByPredicate([Forearm](const FMantleGhostCapsule& C)
			{
				const FString Part = C.Part.ToString();
				return (Part == TEXT("Wrist") && FVector::Dist(C.End, Forearm->End) < 0.01) || (Part.StartsWith(TEXT("Hand_")) && FVector::Dist(C.Start, Forearm->End) < 0.01);
			});
			TestTrue(*FString::Printf(TEXT("%s: the %s forearm ends on its hand's wrist"), Case.What, Side), bJoined);
		}
	}
	return true;
}

#endif
