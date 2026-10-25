// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderKind.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The weapon builder's contact checks: the palm boxes against the gun's geometry, the sight against the top of the gun,
// and the aiming eye. The pure functions run on synthetic point clouds in the weapon frame; no asset needed.

namespace MantleWeaponBuilderContactTestPrivate
{
	/** A box of points about 1 cm apart, corners included. */
	static void AddBox(TArray<FVector3f>& Out, const FVector& Min, const FVector& Max)
	{
		const FVector Size = Max - Min;
		const int32 Nx = FMath::Max(1, FMath::RoundToInt32(Size.X));
		const int32 Ny = FMath::Max(1, FMath::RoundToInt32(Size.Y));
		const int32 Nz = FMath::Max(1, FMath::RoundToInt32(Size.Z));
		for (int32 X = 0; X <= Nx; ++X)
		{
			for (int32 Y = 0; Y <= Ny; ++Y)
			{
				for (int32 Z = 0; Z <= Nz; ++Z)
				{
					Out.Add(FVector3f(Min + FVector(Size.X * X / Nx, Size.Y * Y / Ny, Size.Z * Z / Nz)));
				}
			}
		}
	}

	/** A box gun in the weapon frame: a handguard 30 cm long, 5 wide, from z = 0 (the underside) to z = 9, bore along +X, plus a grip. */
	static TArray<FVector3f> MakeBoxGun()
	{
		TArray<FVector3f> Out;
		AddBox(Out, FVector(10, -2.5, 0), FVector(40, 2.5, 9));
		AddBox(Out, FVector(-1.5, -1.5, -10), FVector(1.5, 1.5, 0));
		return Out;
	}

	/** The same rifle MantleWeaponBuilderKindTest builds, in the mesh's own space (bore along +Y, up +Z). */
	static TArray<FVector3f> MakeMeshRifle()
	{
		TArray<FVector3f> Out;
		AddBox(Out, FVector(-2, -5, -2), FVector(2, 15, 8));
		AddBox(Out, FVector(-2.5, 15, 0), FVector(2.5, 50, 9));
		AddBox(Out, FVector(-1, 50, 4), FVector(1, 80, 6));
		AddBox(Out, FVector(-1.5, 18, -9), FVector(1.5, 24, 0));
		AddBox(Out, FVector(-1.5, 28, -14), FVector(1.5, 34, 0));
		return Out;
	}

	static bool AnyContains(const TArray<FText>& Messages, const TCHAR* Fragment)
	{
		return Messages.ContainsByPredicate([Fragment](const FText& Message) { return Message.ToString().Contains(Fragment); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderPalmInsideTest,
	"Mantle.Editor.WeaponBuilderKind.PalmInside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderPalmInsideTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderContactTestPrivate;
	using namespace MantleWeaponBuilderMath;
	const TArray<FVector3f> Gun = MakeBoxGun();

	// A palm under the gun (thin side vertical) centred in the handguard is deep inside it.
	const FPalmBox InHandguard = MakePalmBox(FVector(25, 0, 4.5), false);
	TestTrue(TEXT("a palm cutting through the handguard's underside is inside the gun"), IsLeftPalmInsideGun(Gun, FTransform::Identity, FVector(25, 0, 1.0)));
	TestFalse(TEXT("a palm 1 cm under the handguard is not"), IsLeftPalmInsideGun(Gun, FTransform::Identity, FVector(25, 0, -1.0)));

	// 1 cm below the underside: resting against the gun, not inside it.
	const FPalmBox Below = MakePalmBox(FVector(25, 0, -1.0), false);
	TestEqual(TEXT("a palm 1 cm under the handguard has nothing inside it"), CountSamplesInsidePalm(Gun, FTransform::Identity, Below), 0);

	// A wrapped palm (thin side facing the gun's side) on the grip's side, clear of it.
	const FPalmBox BesideGrip = MakePalmBox(FVector(0, 6.5, -5), true);
	TestEqual(TEXT("a palm 5 cm off the grip's side has nothing inside it"), CountSamplesInsidePalm(Gun, FTransform::Identity, BesideGrip), 0);

	// The shape of the boxes: thin along Y when wrapped, thin along Z when under the gun.
	TestTrue(TEXT("wrapped palm is thin along Y"), InHandguard.HalfExtent.Z < InHandguard.HalfExtent.Y && BesideGrip.HalfExtent.Y < BesideGrip.HalfExtent.X);
	TestEqual(TEXT("palm length"), InHandguard.HalfExtent.X * 2.0, PalmLengthCm, 1.0e-6);

	// Samples are mapped through the transform: the same gun at half size in mesh units, scale 2 into the weapon frame.
	TArray<FVector3f> Half;
	for (const FVector3f& P : Gun)
	{
		Half.Add(P * 0.5f);
	}
	const FTransform Scaled(FQuat::Identity, FVector::ZeroVector, FVector(2.0));
	TestEqual(TEXT("the count follows the transform"), CountSamplesInsidePalm(Half, Scaled, Below), 0);
	TestTrue(TEXT("and still finds the palm cutting through the handguard"), IsLeftPalmInsideGun(Half, Scaled, FVector(25, 0, 1.0)));

	// Surface samples: a single large triangle gets sampled across, not just at its corners.
	TArray<FVector3f> Samples;
	BuildSurfaceSamples({ FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0) }, { 0, 1, 2 }, 1.0, 1000, Samples);
	TestTrue(TEXT("a 10 cm triangle is sampled about 1 cm apart"), Samples.Num() > 40);
	BuildSurfaceSamples({ FVector3f(0, 0, 0), FVector3f(10, 0, 0), FVector3f(0, 10, 0) }, { 0, 1, 2 }, 1.0, 20, Samples);
	TestTrue(TEXT("the sample count is capped"), Samples.Num() <= 20);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderHandCapsulesTest,
	"Mantle.Editor.WeaponBuilderKind.HandCapsules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderHandCapsulesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderMath;
	auto Find = [](const TArray<FMantleGhostCapsule>& Hand, const TCHAR* Part) -> const FMantleGhostCapsule*
	{
		return Hand.FindByPredicate([Part](const FMantleGhostCapsule& C) { return C.Part == FName(Part); });
	};

	// A right hand on a grip 3 cm wide (1.5 half-width), the hand on its centre line.
	const FVector Palm(0, 0, 0);
	const TArray<FMantleGhostCapsule> Right = MakeHandCapsules(Palm, EHandStyle::GripRight, 1.5, false);
	TestEqual(TEXT("three palm capsules, four fingers and a thumb of two each"), Right.Num(), 3 + 5 * 2);
	for (int32 I = 0; I < 3; ++I)
	{
		TestNotNull(*FString::Printf(TEXT("palm %d"), I), Find(Right, *FString::Printf(TEXT("Palm_%d"), I)));
	}
	// The three wrapping fingers and the thumb end just outside the gun's far side (half-width 1.5), not sticking out.
	const TCHAR* Wrapped[] = { TEXT("Finger_1_B"), TEXT("Finger_2_B"), TEXT("Finger_3_B"), TEXT("Thumb_B") };
	for (const TCHAR* Part : Wrapped)
	{
		if (const FMantleGhostCapsule* Tip = Find(Right, Part))
		{
			const double Outside = -Tip->End.Y - 1.5;
			TestTrue(*FString::Printf(TEXT("%s ends outside the gun's far side but within 3 cm of it"), Part), Outside > 0.0 && Outside < 3.0);
		}
		else
		{
			AddError(FString::Printf(TEXT("missing %s"), Part));
		}
	}
	if (const FMantleGhostCapsule* IndexTip = Find(Right, TEXT("Finger_0_B")))
	{
		TestTrue(TEXT("the index points ahead of the palm along the bore"), IndexTip->End.X > Palm.X + 4.0);
	}

	// A wide gun: the fingers reach as far as they can and no further.
	const TArray<FMantleGhostCapsule> Wide = MakeHandCapsules(Palm, EHandStyle::GripRight, 12.0, false);
	if (const FMantleGhostCapsule* Tip = Find(Wide, TEXT("Finger_2_B")))
	{
		TestTrue(TEXT("a very wide gun is reached up to its limit only"), -Tip->End.Y < HandSegment1Cm + HandSegment2Cm);
	}

	// A two-handed left hand: palm centre on the point, fingers up the +Y side, thumb up the -Y side.
	const FVector Under(25, 0, -1);
	const TArray<FMantleGhostCapsule> Left = MakeHandCapsules(Under, EHandStyle::SupportUnder, 2.5, false);
	TestEqual(TEXT("left hand capsule count"), Left.Num(), 13);
	if (const FMantleGhostCapsule* Middle = Find(Left, TEXT("Palm_1")))
	{
		TestTrue(TEXT("the palm's centre is the hand point"), ((Middle->Start + Middle->End) * 0.5).Equals(Under, 1.0e-6));
	}
	for (int32 I = 0; I < 4; ++I)
	{
		if (const FMantleGhostCapsule* Tip = Find(Left, *FString::Printf(TEXT("Finger_%d_B"), I)))
		{
			TestTrue(TEXT("a finger tip is above the palm on the +Y side"), Tip->End.Z > Under.Z && Tip->End.Y > Under.Y);
		}
	}
	if (const FMantleGhostCapsule* Thumb = Find(Left, TEXT("Thumb_B")))
	{
		TestTrue(TEXT("the thumb tip is on the -Y side and above the palm"), Thumb->End.Y < Under.Y && Thumb->End.Z > Under.Z);
	}

	// The support hand on a pistol is the same hand, rolled.
	TestEqual(TEXT("cupping hand capsule count"), MakeHandCapsules(Under, EHandStyle::SupportCup, 2.5, false).Num(), 13);

	// The warning flag is on every capsule.
	for (const EHandStyle Style : { EHandStyle::GripRight, EHandStyle::SupportUnder, EHandStyle::SupportCup })
	{
		for (const FMantleGhostCapsule& C : MakeHandCapsules(Under, Style, 2.0, true))
		{
			TestTrue(*C.Part.ToString(), C.bWarning);
		}
		for (const FMantleGhostCapsule& C : MakeHandCapsules(Under, Style, 2.0, false))
		{
			TestFalse(*C.Part.ToString(), C.bWarning);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderSightInsideTest,
	"Mantle.Editor.WeaponBuilderKind.SightInside",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderSightInsideTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderContactTestPrivate;
	using namespace MantleWeaponBuilderMath;
	const TArray<FVector3f> Gun = MakeBoxGun();
	double Top = 0.0;

	TestTrue(TEXT("a sight 5 cm below the top is inside the gun"), IsSightInsideGun(Gun, FTransform::Identity, FVector(25, 0, 4), Top));
	TestEqual(TEXT("the top of the gun there"), Top, 9.0, 1.0e-6);
	TestFalse(TEXT("a sight on the top is fine"), IsSightInsideGun(Gun, FTransform::Identity, FVector(25, 0, 9), Top));
	TestFalse(TEXT("0.5 cm under the top is within tolerance"), IsSightInsideGun(Gun, FTransform::Identity, FVector(25, 0, 8.5), Top));
	TestFalse(TEXT("a sight above the gun is fine"), IsSightInsideGun(Gun, FTransform::Identity, FVector(25, 0, 12), Top));
	TestFalse(TEXT("a sight well ahead of the gun has nothing under it"), IsSightInsideGun(Gun, FTransform::Identity, FVector(200, 0, 0), Top));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderAimEyeTest,
	"Mantle.Editor.WeaponBuilderKind.AimEye",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderAimEyeTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderContactTestPrivate;
	using namespace MantleWeaponBuilderMath;

	// Two-handed: 16.5 cm ahead of the rear of the gun, at the sight's side and height.
	const FVector Long = AimEyeWeapon(-12.0, FVector(5, 1, 7), true);
	TestTrue(TEXT("long gun eye"), Long.Equals(FVector(4.5, 1, 7), 1.0e-6));
	// A pistol: 21.6 cm behind the rear.
	const FVector Pistol = AimEyeWeapon(-12.0, FVector(5, 1, 7), false);
	TestTrue(TEXT("pistol eye"), Pistol.Equals(FVector(-33.6, 1, 7), 1.0e-6));

	// Through the kind: the overlay draws the eye and the line only while aiming, and the head leans to the left of the eye.
	FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	const TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeMeshRifle());
	TestEqual(TEXT("no palm boxes"), Kind.Overlay(Points).Boxes.Num(), 0);
	TestEqual(TEXT("the overlay draws no hands: they are the body's"), Kind.Overlay(Points).Capsules.Num(), 0);
	const int32 HandParts = Kind.GhostPose(Points).FilterByPredicate([](const FMantleGhostCapsule& C)
	{
		const FString Part = C.Part.ToString();
		return Part.StartsWith(TEXT("Palm_")) || Part.StartsWith(TEXT("Finger_")) || Part.StartsWith(TEXT("Thumb")) || Part.StartsWith(TEXT("Hand_"));
	}).Num();
	TestEqual(TEXT("two hands of 14 capsules in the body"), HandParts, 28);
	TestEqual(TEXT("no eye marker without aiming"), Kind.Overlay(Points).Markers.Num(), 0);
	Kind.SetShowAiming(true);
	const FMantleBuilderOverlay Aim = Kind.Overlay(Points);
	if (TestEqual(TEXT("an eye marker while aiming"), Aim.Markers.Num(), 1) && TestEqual(TEXT("a line of sight while aiming"), Aim.Lines.Num(), 1))
	{
		const FMantleWeaponFitResult& Fit = Kind.GetLastFit();
		const FVector EyeWeapon = Fit.MeshToWeapon.TransformPosition(Aim.Markers[0].Location);
		TestEqual(TEXT("the eye is 16.5 cm ahead of the rear"), EyeWeapon.X, -double(Fit.GripFromRearCm) + 16.5, 0.05);
		const FMantleBuilderPoint* Sight = MantleBuilderPoints::Find(Points, TEXT("Sight"));
		if (TestNotNull(TEXT("sight"), Sight))
		{
			const FVector SightWeapon = Fit.MeshToWeapon.TransformPosition(Sight->Transform.GetLocation());
			TestEqual(TEXT("the eye is at the sight's height"), EyeWeapon.Z, SightWeapon.Z, 0.05);
			// The line runs from the eye through the sight.
			const FVector Start = Fit.MeshToWeapon.TransformPosition(Aim.Lines[0].Start);
			const FVector End = Fit.MeshToWeapon.TransformPosition(Aim.Lines[0].End);
			FVector Closest = FMath::ClosestPointOnSegment(SightWeapon, Start, End);
			TestTrue(TEXT("the line of sight passes through the sight point"), FVector::Dist(Closest, SightWeapon) < 0.1);
		}
		const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Points);
		const FMantleGhostCapsule* Head = Ghost.FindByPredicate([](const FMantleGhostCapsule& C) { return C.Part == FName(TEXT("Head")); });
		if (TestNotNull(TEXT("head"), Head))
		{
			const FVector Centre = Fit.MeshToWeapon.TransformPosition((Head->Start + Head->End) * 0.5);
			TestTrue(TEXT("aiming: the head is behind the eye"), Centre.X < EyeWeapon.X);
			TestTrue(TEXT("aiming: the head is to the left of the eye"), Centre.Y < EyeWeapon.Y);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderHandsInsideCheckTest,
	"Mantle.Editor.WeaponBuilderKind.HandsInsideCheck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderHandsInsideCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderContactTestPrivate;
	const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeMeshRifle());
	auto Place = [&Points](const TCHAR* Role, const FVector& MeshLocation)
	{
		FMantleBuilderPoint* Point = Points.FindByPredicate([Role](const FMantleBuilderPoint& P) { return P.Role == FName(Role); });
		if (Point)
		{
			Point->Transform.SetLocation(MeshLocation);
			Point->Source = EMantleBuilderPointSource::Creator;
		}
		return Point != nullptr;
	};

	// The mesh's handguard runs along +Y from 15 to 50, z from 0 to 9 (mesh space; the fit keeps this model at scale 1).
	if (TestTrue(TEXT("left hand placed"), Place(TEXT("Grip_L"), FVector(0, 40, 4.5))))
	{
		TestTrue(TEXT("a left hand in the handguard is flagged"), AnyContains(Kind.Check(Points), TEXT("The left hand is inside the gun. Drag it down until it sits under the front of the gun.")));
		const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Points);
		// The body starts with the right hand's 14 capsules (13 and the back of the hand), then the left's 14: red on the left only.
		if (TestTrue(TEXT("two hands lead the body"), Ghost.Num() > 28))
		{
			bool bRightRed = false;
			bool bLeftAllRed = true;
			for (int32 I = 0; I < 28; ++I)
			{
				if (I < 14)
				{
					bRightRed |= Ghost[I].bWarning;
				}
				else
				{
					bLeftAllRed &= Ghost[I].bWarning;
				}
			}
			TestTrue(TEXT("and every capsule of the left hand is red"), bLeftAllRed);
			TestFalse(TEXT("the right hand is not"), bRightRed);
		}
	}
	if (TestTrue(TEXT("left hand moved"), Place(TEXT("Grip_L"), FVector(0, 40, -1.0))))
	{
		TestFalse(TEXT("a left hand 1 cm under the handguard is not"), AnyContains(Kind.Check(Points), TEXT("The left hand is inside the gun")));
	}

	// A sight dropped into the receiver.
	if (TestTrue(TEXT("sight moved"), Place(TEXT("Sight"), FVector(0, 30, 3.0))))
	{
		TestTrue(TEXT("a sight inside the gun is flagged"), AnyContains(Kind.Check(Points), TEXT("The sight point is inside the gun. Move it up onto the top of the sight.")));
	}
	if (TestTrue(TEXT("sight on top"), Place(TEXT("Sight"), FVector(0, 30, 9.0))))
	{
		TestFalse(TEXT("a sight on top of the gun is not"), AnyContains(Kind.Check(Points), TEXT("The sight point is inside the gun")));
	}
	return true;
}

#endif
