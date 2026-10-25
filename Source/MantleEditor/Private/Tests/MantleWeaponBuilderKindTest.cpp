// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponBuilderKind.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The weapon builder kind on the same synthetic rifle MantleWeaponFitTest builds from boxes of points, in
// centimetres along mesh +Y with up +Z: the muzzle tip is at (0, 80, 5), no asset needed.

namespace MantleWeaponBuilderKindTestPrivate
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

	static TArray<FVector3f> MakeRifle()
	{
		TArray<FVector3f> Out;
		AddBox(Out, FVector(-2, -5, -2), FVector(2, 15, 8));
		AddBox(Out, FVector(-2.5, 15, 0), FVector(2.5, 50, 9));
		AddBox(Out, FVector(-1, 50, 4), FVector(1, 80, 6));
		AddBox(Out, FVector(-1.5, 18, -9), FVector(1.5, 24, 0));
		AddBox(Out, FVector(-1.5, 28, -14), FVector(1.5, 34, 0));
		return Out;
	}

	static FMantleBuilderPoint* FindMutable(TArray<FMantleBuilderPoint>& Points, const TCHAR* Role)
	{
		return Points.FindByPredicate([Role](const FMantleBuilderPoint& P) { return P.Role == FName(Role); });
	}

	static bool AnyWarning(const TArray<FMantleGhostCapsule>& Capsules)
	{
		return Capsules.ContainsByPredicate([](const FMantleGhostCapsule& C) { return C.bWarning; });
	}

	/** Whether any capsule on the given side ("_L" or "_R") that is part of an arm is flagged. */
	static bool SideWarned(const TArray<FMantleGhostCapsule>& Capsules, const TCHAR* Suffix)
	{
		for (const FMantleGhostCapsule& Capsule : Capsules)
		{
			const FString Name = Capsule.Part.ToString();
			if (Capsule.bWarning && Name.EndsWith(Suffix))
			{
				return true;
			}
		}
		return false;
	}

	static const FMantleGhostCapsule* FindCapsule(const TArray<FMantleGhostCapsule>& Capsules, const TCHAR* Part)
	{
		return Capsules.FindByPredicate([Part](const FMantleGhostCapsule& C) { return C.Part == FName(Part); });
	}

	/** The distance between a capsule's axis and a segment, minus the capsule's radius: negative when they intersect. */
	static double ClearanceCm(const FMantleGhostCapsule& Capsule, const FVector& SegStart, const FVector& SegEnd)
	{
		FVector OnCapsule, OnSegment;
		FMath::SegmentDistToSegment(Capsule.Start, Capsule.End, SegStart, SegEnd, OnCapsule, OnSegment);
		return FVector::Dist(OnCapsule, OnSegment) - Capsule.RadiusCm;
	}

	/** Head and torso must stay clear of the line from the rear of the gun to the muzzle; the head must also sit to the left of it when shouldered. */
	static void CheckClear(FAutomationTestBase& Test, const TCHAR* What, const TArray<FMantleGhostCapsule>& Ghost, const FVector& Rear, const FVector& Muzzle)
	{
		for (const TCHAR* Part : { TEXT("Head"), TEXT("Torso") })
		{
			if (const FMantleGhostCapsule* Capsule = FindCapsule(Ghost, Part))
			{
				Test.TestTrue(*FString::Printf(TEXT("%s: the %s is clear of the gun (%.1f cm)"), What, Part, ClearanceCm(*Capsule, Rear, Muzzle)), ClearanceCm(*Capsule, Rear, Muzzle) > 0.0);
			}
			else
			{
				Test.AddError(FString::Printf(TEXT("%s: the ghost has no %s"), What, Part));
			}
		}
	}

	static bool AnyContains(const TArray<FText>& Messages, const TCHAR* Fragment)
	{
		return Messages.ContainsByPredicate([Fragment](const FText& Message) { return Message.ToString().Contains(Fragment); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderKindRolesTest,
	"Mantle.Editor.WeaponBuilderKind.Roles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderKindRolesTest::RunTest(const FString& Parameters)
{
	const FMantleWeaponBuilderKind Rifle(EMantleWeaponFitHold::Rifle);
	const TArray<FMantleBuilderPointRole> RifleRoles = Rifle.GetRoles();
	const FMantleBuilderPointRole* GripL = RifleRoles.FindByPredicate([](const FMantleBuilderPointRole& R) { return R.Role == TEXT("Grip_L"); });
	if (TestNotNull(TEXT("a rifle has a left hand role"), GripL))
	{
		TestTrue(TEXT("the left hand is required for a rifle"), GripL->bRequired);
		TestTrue(TEXT("a gun has no mirror"), GripL->MirrorRole == NAME_None);
	}
	TestEqual(TEXT("six roles"), RifleRoles.Num(), 6);
	for (const FMantleBuilderPointRole& Role : RifleRoles)
	{
		TestFalse(*FString::Printf(TEXT("%s has a label"), *Role.Role.ToString()), Role.Label.IsEmpty());
		TestFalse(*FString::Printf(TEXT("%s has help"), *Role.Role.ToString()), Role.Help.IsEmpty());
		TestTrue(*FString::Printf(TEXT("%s hangs on the mesh root"), *Role.Role.ToString()), Role.DefaultParent == NAME_None);
	}

	// The schema makes gripL optional on a one-handed pistol (the support hand of a two-handed hold).
	const FMantleWeaponBuilderKind Pistol(EMantleWeaponFitHold::Pistol);
	const TArray<FMantleBuilderPointRole> PistolRoles = Pistol.GetRoles();
	const FMantleBuilderPointRole* PistolGripL = PistolRoles.FindByPredicate([](const FMantleBuilderPointRole& R) { return R.Role == TEXT("Grip_L"); });
	if (TestNotNull(TEXT("a pistol has a left hand role"), PistolGripL))
	{
		TestFalse(TEXT("the left hand is optional for a pistol"), PistolGripL->bRequired);
	}
	const FMantleBuilderPointRole* Magazine = RifleRoles.FindByPredicate([](const FMantleBuilderPointRole& R) { return R.Role == TEXT("Magazine"); });
	if (TestNotNull(TEXT("a magazine role"), Magazine))
	{
		TestFalse(TEXT("the magazine is optional"), Magazine->bRequired);
		TestFalse(TEXT("the magazine only has a location"), Magazine->bRotates);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderKindGuessTest,
	"Mantle.Editor.WeaponBuilderKind.Guess",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderKindGuessTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderKindTestPrivate;
	const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	const TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeRifle());
	const FMantleWeaponFitResult& Fit = Kind.GetLastFit();

	for (const FMantleBuilderPointRole& Role : Kind.GetRoles())
	{
		if (!Role.bRequired)
		{
			continue;
		}
		const FMantleBuilderPoint* Point = MantleBuilderPoints::Find(Points, Role.Role);
		if (!TestNotNull(*FString::Printf(TEXT("%s is guessed"), *Role.Role.ToString()), Point))
		{
			continue;
		}
		TestTrue(TEXT("a guess is not a creator placement"), Point->Source == EMantleBuilderPointSource::Guess);
		TestTrue(TEXT("Guess equals Transform"), Point->Guess.Equals(Point->Transform, 1.0e-4f));
		TestTrue(TEXT("on the mesh root"), Point->Parent == NAME_None);

		const FTransform* InWeapon = Fit.Points.Find(Role.Role);
		if (TestNotNull(TEXT("the fit has the same point"), InWeapon))
		{
			const FVector Back = Fit.MeshToWeapon.TransformPosition(Point->Transform.GetLocation());
			TestTrue(*FString::Printf(TEXT("%s maps back to the fit's weapon-frame point"), *Role.Role.ToString()), Back.Equals(InWeapon->GetLocation(), 0.01));
		}
	}
	// The mesh is an 85 cm rifle, so the points sit in the unscaled mesh: the muzzle is at the barrel tip.
	if (const FMantleBuilderPoint* Muzzle = MantleBuilderPoints::Find(Points, TEXT("Muzzle")))
	{
		TestTrue(TEXT("the muzzle is at the barrel tip in mesh space"), FVector::Dist(Muzzle->Transform.GetLocation(), FVector(0, 80, 5)) < 1.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderKindGhostTest,
	"Mantle.Editor.WeaponBuilderKind.GhostPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderKindGhostTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderKindTestPrivate;
	const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeRifle());
	TestEqual(TEXT("the 85 cm rifle keeps scale 1"), Kind.GetLastFit().Scale, 1.f, 1.0e-3f);

	const TArray<FMantleGhostCapsule> Held = Kind.GhostPose(Points);
	TestTrue(TEXT("the ghost has capsules"), Held.Num() > 0);
	TestFalse(TEXT("both hands reach their points"), AnyWarning(Held));
	// The butt sits in the shoulder: the right shoulder joint is just right of and behind the butt's middle, at its height.
	if (const FMantleGhostCapsule* UpperArm = FindCapsule(Held, TEXT("UpperArm_R")))
	{
		const FTransform& ToWeapon = Kind.GetLastFit().MeshToWeapon;
		const FVector Joint = ToWeapon.TransformPosition(UpperArm->Start);
		const double RearX = -Kind.GetLastFit().GripFromRearCm;
		AddInfo(FString::Printf(TEXT("right shoulder joint in the weapon frame %s, gun rear at x %.2f"), *Joint.ToCompactString(), RearX));
		TestTrue(TEXT("the shoulder joint is just behind the butt"), Joint.X <= RearX && Joint.X >= RearX - 5.0);
		TestTrue(TEXT("the shoulder joint is a few cm to the right of the bore"), Joint.Y > 2.0 && Joint.Y < 9.0);
	}
	// The middle of each hand is on its point (the palm), so the end of the arm the creator sees is the hand, not the forearm.
	for (const TCHAR* Side : { TEXT("R"), TEXT("L") })
	{
		const FMantleBuilderPoint* Grip = Points.FindByPredicate([Side](const FMantleBuilderPoint& P) { return P.Role == FName(FString::Printf(TEXT("Grip_%s"), Side)); });
		const FMantleGhostCapsule* Hand = FindCapsule(Held, *FString::Printf(TEXT("Hand_%s"), Side));
		if (TestNotNull(*FString::Printf(TEXT("Grip_%s and Hand_%s exist"), Side, Side), Grip) && Hand)
		{
			const double Off = FVector::Dist((Hand->Start + Hand->End) * 0.5, Grip->Transform.GetLocation());
			TestTrue(*FString::Printf(TEXT("the middle of Hand_%s is on its point (%.2f cm off)"), Side, Off), Off <= 1.0);
		}
	}
	// Contact warnings ("... is inside the gun") are tested on their own in MantleWeaponBuilderContactTest; this is about missing points and reach.
	int32 NonContactWarnings = 0;
	for (const FText& Warning : Kind.Check(Points))
	{
		NonContactWarnings += Warning.ToString().Contains(TEXT("inside the gun")) ? 0 : 1;
	}
	TestEqual(TEXT("a guess with every hand placed is clean"), NonContactWarnings, 0);

	if (FMantleBuilderPoint* GripL = FindMutable(Points, TEXT("Grip_L")))
	{
		GripL->Transform.SetLocation(GripL->Transform.GetLocation() + FVector(0, 0, 200));
		GripL->Source = EMantleBuilderPointSource::Creator;
	}
	const TArray<FMantleGhostCapsule> Far = Kind.GhostPose(Points);
	TestTrue(TEXT("the left arm is flagged when its hand is 2 m away"), SideWarned(Far, TEXT("_L")));
	TestFalse(TEXT("the right arm is not"), SideWarned(Far, TEXT("_R")));
	TestTrue(TEXT("Check says the left hand cannot reach"), AnyContains(Kind.Check(Points), TEXT("The left hand can't reach that spot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderKindHoldTest,
	"Mantle.Editor.WeaponBuilderKind.GhostHoldClearsGun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderKindHoldTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderKindTestPrivate;

	// A rifle: the stock at the right shoulder, the head to the left of the bore line, the torso beside it, both hands reaching.
	{
		const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
		const TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeRifle());
		const FMantleWeaponFitResult& Fit = Kind.GetLastFit();
		const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Points);
		TestFalse(TEXT("rifle: both hands still reach"), AnyWarning(Ghost));
		const FMantleBuilderPoint* Muzzle = MantleBuilderPoints::Find(Points, TEXT("Muzzle"));
		if (TestNotNull(TEXT("rifle: muzzle"), Muzzle))
		{
			// Mesh space is the fit's weapon frame here (scale 1), so map both ends through it.
			const FVector Rear = Fit.MeshToWeapon.InverseTransformPosition(FVector(-Fit.GripFromRearCm, 0.0, Fit.Points.FindRef(TEXT("Muzzle")).GetLocation().Z));
			CheckClear(*this, TEXT("rifle"), Ghost, Rear, Muzzle->Transform.GetLocation());
			if (const FMantleGhostCapsule* Head = FindCapsule(Ghost, TEXT("Head")))
			{
				// In the weapon frame +Y is the shooter's right, so the head's centre is on the negative side of the bore line.
				const FVector Centre = Fit.MeshToWeapon.TransformPosition((Head->Start + Head->End) * 0.5);
				TestTrue(TEXT("rifle: the head sits to the left of the bore line"), Centre.Y < Fit.Points.FindRef(TEXT("Muzzle")).GetLocation().Y - 2.0);
				const FMantleBuilderPoint* Sight = MantleBuilderPoints::Find(Points, TEXT("Sight"));
				if (TestNotNull(TEXT("rifle: sight"), Sight))
				{
					TestTrue(TEXT("rifle: the head is behind the sight"), Centre.X < Fit.MeshToWeapon.TransformPosition(Sight->Transform.GetLocation()).X);
				}
			}
		}
	}

	// A pistol held out in both hands, points placed by hand in the weapon frame (the fit is the identity here).
	{
		const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Pistol);
		TArray<FMantleBuilderPoint> Points;
		auto Add = [&Points](const TCHAR* Role, const FVector& Location)
		{
			FMantleBuilderPoint& Point = Points.AddDefaulted_GetRef();
			Point.Role = FName(Role);
			Point.Transform = FTransform(Location);
			Point.Guess = Point.Transform;
			Point.Source = EMantleBuilderPointSource::Guess;
		};
		Add(TEXT("Grip_R"), FVector(0, 0, 0));
		Add(TEXT("Grip_L"), FVector(3, -1, -1));
		Add(TEXT("Muzzle"), FVector(15, 0, 4));
		Add(TEXT("Sight"), FVector(7, 0, 6));
		const TArray<FMantleGhostCapsule> Ghost = Kind.GhostPose(Points);
		TestFalse(TEXT("pistol: both hands still reach"), AnyWarning(Ghost));
		CheckClear(*this, TEXT("pistol"), Ghost, FVector(-2, 0, 0), FVector(15, 0, 4));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponBuilderKindCheckTest,
	"Mantle.Editor.WeaponBuilderKind.Check",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponBuilderKindCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponBuilderKindTestPrivate;
	const FMantleWeaponBuilderKind Kind(EMantleWeaponFitHold::Rifle);
	TArray<FMantleBuilderPoint> Points = Kind.GuessPointsFromPositions(MakeRifle());

	// A muzzle behind the right hand, as if the gun faced backwards.
	const FMantleBuilderPoint* GripR = MantleBuilderPoints::Find(Points, TEXT("Grip_R"));
	FMantleBuilderPoint* Muzzle = FindMutable(Points, TEXT("Muzzle"));
	if (TestNotNull(TEXT("Grip_R"), GripR) && TestNotNull(TEXT("Muzzle"), Muzzle))
	{
		const FVector Fire = Muzzle->Transform.GetRotation().GetForwardVector();
		Muzzle->Transform.SetLocation(GripR->Transform.GetLocation() - Fire * 30.0);
		Muzzle->Source = EMantleBuilderPointSource::Creator;
		TestTrue(TEXT("Check says to flip the gun"), AnyContains(Kind.Check(Points), TEXT("The muzzle is behind the grip. Use Flip")));
	}

	// A missing required point is asked for in plain words.
	TArray<FMantleBuilderPoint> NoLeft = Kind.GuessPointsFromPositions(MakeRifle());
	NoLeft.RemoveAll([](const FMantleBuilderPoint& P) { return P.Role == TEXT("Grip_L"); });
	TestTrue(TEXT("Check asks for the left hand"), AnyContains(Kind.Check(NoLeft), TEXT("Place the left hand.")));

	// A sight dropped below the barrel.
	TArray<FMantleBuilderPoint> LowSight = Kind.GuessPointsFromPositions(MakeRifle());
	const FMantleBuilderPoint* Bore = MantleBuilderPoints::Find(LowSight, TEXT("Muzzle"));
	FMantleBuilderPoint* Sight = FindMutable(LowSight, TEXT("Sight"));
	if (TestNotNull(TEXT("Muzzle"), Bore) && TestNotNull(TEXT("Sight"), Sight))
	{
		const FVector Up = Bore->Transform.GetRotation().GetUpVector();
		Sight->Transform.SetLocation(Bore->Transform.GetLocation() - Up * 5.0);
		TestTrue(TEXT("Check says the sight is low"), AnyContains(Kind.Check(LowSight), TEXT("The sight is below the barrel")));
	}
	return true;
}

#endif
