// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The weapon fit on synthetic guns built in code from boxes of points, so no asset is needed. The shape is a
// rifle in centimetres along mesh +Y with up +Z, the way the pack TR15 is modelled:
//   stock     y -5..15   z -2..8     receiver  y 15..50  z 0..9     barrel  y 50..80  z 4..6
//   pistol grip y 18..24 z -9..0     magazine  y 28..34  z -14..0
// so the muzzle tip is at (0, 80, 5), the grip box centre (0, 21, -4.5) and the magazine box centre (0, 31, -7).

namespace MantleWeaponFitTestPrivate
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
					const FVector P = Min + FVector(Size.X * X / Nx, Size.Y * Y / Ny, Size.Z * Z / Nz);
					Out.Add(FVector3f(P));
				}
			}
		}
	}

	/** The rifle, uniformly scaled by Unit (0.01 for a mesh made in metres) and then rotated. */
	static TArray<FVector3f> MakeRifle(double Unit, const FQuat& Rotation)
	{
		TArray<FVector3f> Local;
		AddBox(Local, FVector(-2, -5, -2), FVector(2, 15, 8));
		AddBox(Local, FVector(-2.5, 15, 0), FVector(2.5, 50, 9));
		AddBox(Local, FVector(-1, 50, 4), FVector(1, 80, 6));
		AddBox(Local, FVector(-1.5, 18, -9), FVector(1.5, 24, 0));
		AddBox(Local, FVector(-1.5, 28, -14), FVector(1.5, 34, 0));
		TArray<FVector3f> Out;
		for (const FVector3f& P : Local)
		{
			Out.Add(FVector3f(Rotation.RotateVector(FVector(P) * Unit)));
		}
		return Out;
	}

	static FVector ToMesh(const FVector& LocalCm, double Unit, const FQuat& Rotation)
	{
		return Rotation.RotateVector(LocalCm * Unit);
	}

	static bool PointLocation(const FMantleWeaponFitResult& Fit, const TCHAR* Name, FVector& OutLocation)
	{
		const FTransform* Found = Fit.Points.Find(FName(Name));
		if (!Found)
		{
			return false;
		}
		OutLocation = Found->GetLocation();
		return true;
	}

	/** The checks every orientation and unit of the rifle has to pass. */
	static void CheckRifle(FAutomationTestBase& Test, const FMantleWeaponFitResult& Fit, double Unit, const FQuat& Rotation)
	{
		FVector Muzzle, GripR, GripL, Magazine;
		if (!Test.TestTrue(TEXT("Muzzle and Grip_R exist"), PointLocation(Fit, TEXT("Muzzle"), Muzzle) && PointLocation(Fit, TEXT("Grip_R"), GripR)))
		{
			return;
		}
		const FVector Tip = Fit.MeshToWeapon.TransformPosition(ToMesh(FVector(0, 80, 5), Unit, Rotation));
		const FVector Butt = Fit.MeshToWeapon.TransformPosition(ToMesh(FVector(0, -5, 3), Unit, Rotation));
		Test.TestTrue(TEXT("the bore runs along +X: the tip is far ahead of the butt"), Tip.X - Butt.X > 80.0);
		Test.TestTrue(TEXT("the muzzle is at the barrel tip, within 1 cm"), FVector::Dist(Muzzle, Tip) < 1.0);
		Test.TestTrue(TEXT("Grip_R is the origin"), GripR.IsNearlyZero(1.0e-3));

		const FVector GripBox = Fit.MeshToWeapon.TransformPosition(ToMesh(FVector(0, 21, -4.5), Unit, Rotation));
		Test.TestTrue(TEXT("Grip_R (the origin) is near the pistol-grip box"), GripBox.Size() < 4.0);
		const FVector Top = Fit.MeshToWeapon.TransformPosition(ToMesh(FVector(0, 30, 9), Unit, Rotation));
		Test.TestTrue(TEXT("up is +Z: the receiver top is above the grip box"), Top.Z > GripBox.Z + 5.0);

		if (Test.TestTrue(TEXT("Grip_L exists for a rifle"), PointLocation(Fit, TEXT("Grip_L"), GripL)))
		{
			Test.TestTrue(TEXT("Grip_L is well ahead of Grip_R"), GripL.X > GripR.X + 20.0);
			Test.TestTrue(TEXT("Grip_L is short of the muzzle"), GripL.X < Muzzle.X);
		}
		if (Test.TestTrue(TEXT("a Magazine guess exists"), PointLocation(Fit, TEXT("Magazine"), Magazine)))
		{
			const FVector MagBox = Fit.MeshToWeapon.TransformPosition(ToMesh(FVector(0, 31, -7), Unit, Rotation));
			Test.TestTrue(TEXT("the Magazine guess is near the magazine box"), FVector::Dist(Magazine, MagBox) < 4.0);
		}
		Test.TestTrue(TEXT("a Sight guess exists"), Fit.Points.Contains(FName(TEXT("Sight"))));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitRifleTest,
	"Mantle.Editor.WeaponFit.Rifle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitRifleTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	FMantleWeaponFitInput In;
	In.Hold = EMantleWeaponFitHold::Rifle;
	In.Positions = MakeRifle(1.0, FQuat::Identity);
	const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);

	TestEqual(TEXT("an 85 cm rifle keeps scale 1"), Fit.Scale, 1.f, 1.0e-3f);
	TestFalse(TEXT("85 cm is inside the rifle range"), Fit.bOutsideTypicalRange);
	TestTrue(TEXT("the scale reason says it was kept"), Fit.ScaleReason.StartsWith(TEXT("Kept at 1.0x")));
	CheckRifle(*this, Fit, 1.0, FQuat::Identity);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitMetresTest,
	"Mantle.Editor.WeaponFit.MeshInMetres",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitMetresTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	FMantleWeaponFitInput In;
	In.Hold = EMantleWeaponFitHold::Rifle;
	In.Positions = MakeRifle(0.01, FQuat::Identity);
	const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);

	TestEqual(TEXT("a mesh in metres is scaled up 100x"), Fit.Scale, 100.f, 0.01f);
	TestTrue(TEXT("the reason mentions metres"), Fit.ScaleReason.Contains(TEXT("metres")));
	TestFalse(TEXT("after the unit fix it is not flagged"), Fit.bOutsideTypicalRange);
	CheckRifle(*this, Fit, 0.01, FQuat::Identity);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitRotatedTest,
	"Mantle.Editor.WeaponFit.ArbitraryRotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitRotatedTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	const FQuat Rotation = FQuat(FRotator(37.0, 112.0, -58.0));
	FMantleWeaponFitInput Base;
	Base.Hold = EMantleWeaponFitHold::Rifle;
	Base.Positions = MakeRifle(1.0, FQuat::Identity);
	FMantleWeaponFitInput Turned = Base;
	Turned.Positions = MakeRifle(1.0, Rotation);

	const FMantleWeaponFitResult FitBase = FMantleWeaponFit::Fit(Base);
	const FMantleWeaponFitResult FitTurned = FMantleWeaponFit::Fit(Turned);
	CheckRifle(*this, FitTurned, 1.0, Rotation);

	TestEqual(TEXT("the same points exist"), FitTurned.Points.Num(), FitBase.Points.Num());
	for (const TPair<FName, FTransform>& Pair : FitBase.Points)
	{
		const FTransform* Other = FitTurned.Points.Find(Pair.Key);
		if (TestNotNull(*FString::Printf(TEXT("%s exists when rotated"), *Pair.Key.ToString()), Other))
		{
			// Bin edges shift by float noise, so grips and the magazine get 2 cm; the muzzle 1 cm.
			const double Tolerance = Pair.Key == FName(TEXT("Muzzle")) ? 1.0 : 2.0;
			TestTrue(*FString::Printf(TEXT("%s is where it was, within %.0f cm"), *Pair.Key.ToString(), Tolerance),
				FVector::Dist(Pair.Value.GetLocation(), Other->GetLocation()) < Tolerance);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitWrongHoldTest,
	"Mantle.Editor.WeaponFit.PistolLengthAsRifleSuggestsPistol",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitWrongHoldTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	FMantleWeaponFitInput In;
	In.Hold = EMantleWeaponFitHold::Rifle;
	In.Positions = MakeRifle(0.24, FQuat::Identity);	// about 20 cm long
	const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);

	TestTrue(TEXT("a 20 cm gun is flagged for a rifle"), Fit.bOutsideTypicalRange);
	TestTrue(TEXT("the suggestion names the pistol"), Fit.Suggestion.Contains(TEXT("pistol")));
	TestEqual(TEXT("it is never scaled to fit"), Fit.Scale, 1.f, 1.0e-3f);
	TestTrue(TEXT("it still returns a muzzle"), Fit.Points.Contains(FName(TEXT("Muzzle"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitMirroredTest,
	"Mantle.Editor.WeaponFit.MirroredInputStaysProperRotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitMirroredTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	const TArray<FVector3f> Source = MakeRifle(1.0, FQuat::Identity);

	// Swapping two axes, or negating one, mirrors the gun.
	for (int32 Variant = 0; Variant < 3; ++Variant)
	{
		FMantleWeaponFitInput In;
		In.Hold = EMantleWeaponFitHold::Rifle;
		for (const FVector3f& P : Source)
		{
			if (Variant == 0)
			{
				In.Positions.Add(FVector3f(P.Y, P.X, P.Z));
			}
			else if (Variant == 1)
			{
				In.Positions.Add(FVector3f(-P.X, P.Y, P.Z));
			}
			else
			{
				In.Positions.Add(FVector3f(P.X, -P.Y, P.Z));
			}
		}
		const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);
		const FQuat Rotation = Fit.MeshToWeapon.GetRotation();
		const FVector A = Rotation.RotateVector(FVector::XAxisVector);
		const FVector B = Rotation.RotateVector(FVector::YAxisVector);
		const FVector C = Rotation.RotateVector(FVector::ZAxisVector);
		TestTrue(*FString::Printf(TEXT("variant %d: the rotation has determinant +1"), Variant), FMath::IsNearlyEqual(A | (B ^ C), 1.0, 1.0e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitLowStockTest,
	"Mantle.Editor.WeaponFit.LowStockStaysUpright",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitLowStockTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	// The rifle plus a stock box at the rear whose centre (z -1) sits 6 cm below the barrel line (z 5), as on a real rifle.
	FMantleWeaponFitInput In;
	In.Hold = EMantleWeaponFitHold::Rifle;
	In.Positions = MakeRifle(1.0, FQuat::Identity);
	TArray<FVector3f> Stock;
	AddBox(Stock, FVector(-2, -25, -4), FVector(2, -5, 2));
	In.Positions.Append(Stock);
	const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);

	const FVector Tip = Fit.MeshToWeapon.TransformPosition(FVector(0, 80, 5));
	const FVector GripBox = Fit.MeshToWeapon.TransformPosition(FVector(0, 21, -4.5));
	const FVector Top = Fit.MeshToWeapon.TransformPosition(FVector(0, 30, 9));
	TestTrue(TEXT("the muzzle end is still ahead"), Tip.X > GripBox.X + 40.0);
	TestTrue(TEXT("the grip box stays below the bore"), GripBox.Z < Tip.Z - 5.0);
	TestTrue(TEXT("the receiver top stays above the grip box"), Top.Z > GripBox.Z + 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitBladePalmTest,
	"Mantle.Editor.WeaponFit.BladePalmBesideHandle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitBladePalmTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	// A sword along mesh +Y: pommel and handle y -2..15, a crossguard at y 15..17 across z, the blade y 17..90.
	FMantleWeaponFitInput In;
	In.Hold = EMantleWeaponFitHold::Melee1h;
	AddBox(In.Positions, FVector(-1, -2, -1), FVector(1, 15, 1));
	AddBox(In.Positions, FVector(-1, 15, -8), FVector(1, 17, 8));
	AddBox(In.Positions, FVector(-0.3, 17, -2.5), FVector(0.3, 90, 2.5));
	const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);

	FVector GripR;
	if (!TestTrue(TEXT("Grip_R exists"), PointLocation(Fit, TEXT("Grip_R"), GripR)))
	{
		return true;
	}
	// The handle's centre line, where Grip_R's distance along it crosses it.
	const FVector Handle = Fit.MeshToWeapon.TransformPosition(FVector(0, Fit.MeshToWeapon.InverseTransformPosition(GripR).Y, 0));
	TestTrue(TEXT("the handle points to the tip (+X)"), Fit.MeshToWeapon.TransformPosition(FVector(0, 90, 0)).X > Handle.X + 60.0);
	TestTrue(TEXT("Grip_R (the palm) is on the handle"), GripR.X > Fit.MeshToWeapon.TransformPosition(FVector(0, -2, 0)).X
		&& GripR.X < Fit.MeshToWeapon.TransformPosition(FVector(0, 15, 0)).X);
	TestTrue(TEXT("the palm is BladePalmFromHandleCm to the handle's -Z"),
		(GripR - Handle).Equals(FVector(0, 0, -FMantleWeaponFit::BladePalmFromHandleCm), 0.05));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitThrowableHandleTest,
	"Mantle.Editor.WeaponFit.ThrowableHeldByHandle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitThrowableHandleTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	// A knife along mesh +Y: handle y -2..10, a guard y 10..12 across z, the blade y 12..56. Whichever way the file faces, the hand point is on the handle
	// (the end with the guard), not at the middle, and the tip is far ahead of it.
	for (const bool bFacingBack : { false, true })
	{
		FMantleWeaponFitInput In;
		In.Hold = EMantleWeaponFitHold::Throwable;
		TArray<FVector3f> Local;
		AddBox(Local, FVector(-1.2, -2, -1.2), FVector(1.2, 10, 1.2));
		AddBox(Local, FVector(-0.8, 10, -5), FVector(0.8, 12, 5));
		AddBox(Local, FVector(-0.3, 12, -1.8), FVector(0.3, 56, 1.8));
		const FQuat Rotation = bFacingBack ? FQuat(FVector::XAxisVector, UE_PI) : FQuat::Identity;
		for (const FVector3f& P : Local)
		{
			In.Positions.Add(FVector3f(Rotation.RotateVector(FVector(P))));
		}
		const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);
		FVector GripR, Tip;
		if (!TestTrue(TEXT("Grip_R and Tip exist"), PointLocation(Fit, TEXT("Grip_R"), GripR) && PointLocation(Fit, TEXT("Tip"), Tip)))
		{
			continue;
		}
		const double Pommel = Fit.MeshToWeapon.TransformPosition(Rotation.RotateVector(FVector(0, -2, 0))).X;
		const double Guard = Fit.MeshToWeapon.TransformPosition(Rotation.RotateVector(FVector(0, 10, 0))).X;
		TestTrue(TEXT("Grip_R is the origin"), GripR.IsNearlyZero(1.0e-3));
		TestTrue(TEXT("the hand is on the handle, between the pommel and the guard"), GripR.X > Pommel && GripR.X < Guard);
		TestTrue(TEXT("the tip is most of the knife ahead of the hand"), Tip.X - GripR.X > 45.0);
	}

	// A grenade (a compact box, 8 x 6 x 6) has no handle: the hand is at the middle, half its length behind the tip.
	FMantleWeaponFitInput Grenade;
	Grenade.Hold = EMantleWeaponFitHold::Throwable;
	AddBox(Grenade.Positions, FVector(-3, -4, -3), FVector(3, 4, 3));
	const FMantleWeaponFitResult GrenadeFit = FMantleWeaponFit::Fit(Grenade);
	FVector GrenadeGrip, GrenadeTip;
	if (TestTrue(TEXT("the grenade has Grip_R and Tip"), PointLocation(GrenadeFit, TEXT("Grip_R"), GrenadeGrip) && PointLocation(GrenadeFit, TEXT("Tip"), GrenadeTip)))
	{
		TestTrue(TEXT("a compact throwable is held at its middle"), FMath::IsNearlyEqual(GrenadeTip.X - GrenadeGrip.X, 4.0, 0.3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitBowTest,
	"Mantle.Editor.WeaponFit.BowHandAndString",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitBowTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	// A bow shaped like the BowAndArrow pack's, in centimetres in its own frame: a front (a) toward the target, a thickness (b) and a
	// length (c). The riser is 16 cm of the middle (a -6..1.8, b +-1.8), the limbs curve toward the archer to tips at a -25.5 (27.3 deep, 135 long).
	// Whichever way the file faces, the front is the riser's side, the palm is behind the riser's front face and past its hand-side face, and the
	// string is just inside the limb tips with the nock a little above the middle.
	struct FCase { const TCHAR* Name; double Unit; FQuat Rotation; bool bFlip; bool bFrontIsRiser; };
	const FCase Cases[] = {
		{TEXT("as modelled"), 1.0, FQuat::Identity, false, true},
		{TEXT("turned around"), 1.0, FQuat(FVector::YAxisVector, UE_PI), false, true},
		{TEXT("tipped and in metres"), 0.01, FQuat(FRotator(20.0, 35.0, 50.0)), false, true},
		{TEXT("flipped by the creator"), 1.0, FQuat::Identity, true, false},
	};
	for (const FCase& Case : Cases)
	{
		FMantleWeaponFitInput In;
		In.Hold = EMantleWeaponFitHold::Bow;
		In.bReverseLength = Case.bFlip;
		TArray<FVector3f> Local;
		AddBox(Local, FVector(-6, -1.8, -8), FVector(1.8, 1.8, 8));
		for (const double Side : { 1.0, -1.0 })
		{
			const auto Limb = [&](double C0, double C1, double A0, double A1)
			{
				AddBox(Local, FVector(A0, -1.8, FMath::Min(Side * C0, Side * C1)), FVector(A1, 1.8, FMath::Max(Side * C0, Side * C1)));
			};
			Limb(8, 25, -10, -4);
			Limb(25, 45, -18, -12);
			Limb(45, 67.5, -25.5, -19);
		}
		for (const FVector3f& P : Local)
		{
			In.Positions.Add(FVector3f(Case.Rotation.RotateVector(FVector(P) * Case.Unit)));
		}
		const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);
		FVector GripL, StringTop, StringBottom, Nock;
		if (!TestTrue(*FString::Printf(TEXT("%s: the bow has Grip_L, both string ends and the nock"), Case.Name),
			PointLocation(Fit, TEXT("Grip_L"), GripL) && PointLocation(Fit, TEXT("StringTop"), StringTop)
			&& PointLocation(Fit, TEXT("StringBottom"), StringBottom) && PointLocation(Fit, TEXT("Nock"), Nock)))
		{
			continue;
		}
		const auto ToWeapon = [&](const FVector& LocalCm) { return Fit.MeshToWeapon.TransformPosition(ToMesh(LocalCm, Case.Unit, Case.Rotation)); };
		TestTrue(*FString::Printf(TEXT("%s: Grip_L is the origin"), Case.Name), GripL.IsNearlyZero(1.0e-3));
		const FVector Front = ToWeapon(FVector(1.8, 0, 0));
		const FVector Tips = ToWeapon(FVector(-25.5, 0, 67.5));
		if (Case.bFrontIsRiser)
		{
			TestTrue(*FString::Printf(TEXT("%s: X is the firing direction, the riser's front 27 cm ahead of the tips"), Case.Name),
				FMath::IsNearlyEqual(Front.X - Tips.X, 27.3, 0.6));
			TestTrue(*FString::Printf(TEXT("%s: the palm is BowPalmBehindFrontCm behind the riser's front face"), Case.Name),
				FMath::IsNearlyEqual(Front.X, FMantleWeaponFit::BowPalmBehindFrontCm, 0.6));
			TestTrue(*FString::Printf(TEXT("%s: the string plane is just inside the limb tips"), Case.Name),
				FMath::IsNearlyEqual(StringTop.X - Tips.X, 2.4, 0.5) && FMath::IsNearlyEqual(StringBottom.X, StringTop.X, 1.0e-3)
				&& FMath::IsNearlyEqual(Nock.X - Tips.X, 2.9, 0.5));
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the creator's Flip puts the riser at the back"), Case.Name), Front.X < Tips.X - 20.0);
			continue;
		}
		const double MiddleZ = ToWeapon(FVector::ZeroVector).Z;
		TestTrue(*FString::Printf(TEXT("%s: the palm is 5.6 %% of the length (7.6 cm) below the middle"), Case.Name),
			FMath::IsNearlyEqual(FMath::Abs(MiddleZ), 7.56, 0.7));
		TestTrue(*FString::Printf(TEXT("%s: the string runs the length of the limbs"), Case.Name),
			FMath::IsNearlyEqual(FMath::Abs(StringTop.Z - StringBottom.Z), 135.0, 2.0) && (StringTop.Z - Nock.Z) * (StringBottom.Z - Nock.Z) < 0.0);
		TestTrue(*FString::Printf(TEXT("%s: the nock is 2 cm above the middle"), Case.Name),
			FMath::IsNearlyEqual(FMath::Abs(Nock.Z - MiddleZ), 2.0, 0.5));
		// The palm is past the riser's hand-side face, the arrow on the other face.
		const double FaceA = ToWeapon(FVector(0, -1.8, 0)).Y;
		const double FaceB = ToWeapon(FVector(0, 1.8, 0)).Y;
		const double HandFace = FMath::Min(FaceA, FaceB);
		const double ArrowFace = FMath::Max(FaceA, FaceB);
		TestTrue(*FString::Printf(TEXT("%s: the riser's hand-side face is BowPalmBeyondRiserCm from the palm"), Case.Name),
			FMath::IsNearlyEqual(HandFace, FMantleWeaponFit::BowPalmBeyondRiserCm, 0.6));
		TestTrue(*FString::Printf(TEXT("%s: the nock lies on the riser's other face"), Case.Name),
			FMath::IsNearlyEqual(Nock.Y, ArrowFace - FMantleWeaponFit::BowNockInsideRiserCm, 0.6) && FMath::IsNearlyEqual(StringTop.Y, Nock.Y, 1.0e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponFitStaffTest,
	"Mantle.Editor.WeaponFit.StaffHeadAndGrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponFitStaffTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponFitTestPrivate;
	// A staff shaped like the MagicStaff pack's, in centimetres in its own frame: a plain shaft from the butt at -119 to 10 on
	// the line y 0, z 0, a neck curling up 5 cm, and a bulky orb at the top (62 cm), 181 cm in all. Whichever way the file faces, the head is the
	// bulkier end and the front, the palm is on the shaft 0.65 of the length from the butt, the Focus is the middle of the head's top and the
	// left hand's point is a sixth of the length back toward the butt. The creator's Flip puts the head at the other end.
	struct FCase { const TCHAR* Name; double Unit; FQuat Rotation; bool bFlip; };
	const FCase Cases[] = {
		{TEXT("as modelled"), 1.0, FQuat::Identity, false},
		{TEXT("turned around"), 1.0, FQuat(FVector::ZAxisVector, UE_PI), false},
		{TEXT("tipped and in metres"), 0.01, FQuat(FRotator(20.0, 35.0, 50.0)), false},
		{TEXT("flipped by the creator"), 1.0, FQuat::Identity, true},
	};
	for (const FCase& Case : Cases)
	{
		FMantleWeaponFitInput In;
		In.Hold = EMantleWeaponFitHold::Staff;
		In.bReverseLength = Case.bFlip;
		TArray<FVector3f> Local;
		AddBox(Local, FVector(-119, -1.5, -1.5), FVector(10, 1.5, 1.5));
		AddBox(Local, FVector(10, -1.5, -1.5), FVector(48, 1.5, 5.3));
		AddBox(Local, FVector(48, -6, -3), FVector(62, 6, 9));
		for (const FVector3f& P : Local)
		{
			In.Positions.Add(FVector3f(Case.Rotation.RotateVector(FVector(P) * Case.Unit)));
		}
		const FMantleWeaponFitResult Fit = FMantleWeaponFit::Fit(In);
		FVector GripR, GripL, Focus;
		if (!TestTrue(*FString::Printf(TEXT("%s: the staff has both grips and a focus"), Case.Name),
			PointLocation(Fit, TEXT("Grip_R"), GripR) && PointLocation(Fit, TEXT("Grip_L"), GripL) && PointLocation(Fit, TEXT("Focus"), Focus)))
		{
			continue;
		}
		const auto ToWeapon = [&](const FVector& LocalCm) { return Fit.MeshToWeapon.TransformPosition(ToMesh(LocalCm, Case.Unit, Case.Rotation)); };
		const double Length = 181.0;
		TestTrue(*FString::Printf(TEXT("%s: Grip_R is the origin"), Case.Name), GripR.IsNearlyZero(1.0e-3));
		const FVector Butt = ToWeapon(FVector(-119, 0, 0));
		const FVector HeadTop = ToWeapon(FVector(62, 0, 3));
		if (!Case.bFlip)
		{
			TestTrue(*FString::Printf(TEXT("%s: X runs butt to head, the palm 0.65 of the length from the butt"), Case.Name),
				FMath::IsNearlyEqual(Butt.X, -FMantleWeaponFit::StaffGripFromButtShare * Length, 1.5) && FMath::IsNearlyEqual(HeadTop.X, (1.0 - FMantleWeaponFit::StaffGripFromButtShare) * Length, 1.5));
			TestTrue(*FString::Printf(TEXT("%s: the Focus is the middle of the head's top"), Case.Name), (Focus - HeadTop).Size() < 1.5);
			TestTrue(*FString::Printf(TEXT("%s: the left hand's point is a sixth of the length back"), Case.Name),
				FMath::IsNearlyEqual(GripL.X, -FMantleWeaponFit::StaffLeftHandBehindShare * Length, 1.5) && FMath::Abs(GripL.Y) < 1.0e-3 && FMath::Abs(GripL.Z) < 1.0e-3);
		}
		else
		{
			TestTrue(*FString::Printf(TEXT("%s: the creator's Flip puts the head at the back"), Case.Name),
				HeadTop.X < Butt.X && FMath::IsNearlyEqual(HeadTop.X, -FMantleWeaponFit::StaffGripFromButtShare * Length, 1.5));
		}
		// The palm is on the shaft's line, not the middle of the box that the curl pulls up: 0.65 of the length from the butt (-119), or with
		// the creator's Flip from the head's end (62).
		const double PalmX = Case.bFlip ? 62.0 - FMantleWeaponFit::StaffGripFromButtShare * Length : -119.0 + FMantleWeaponFit::StaffGripFromButtShare * Length;
		const FVector OnShaft = ToWeapon(FVector(PalmX, 0, 0));
		TestTrue(*FString::Printf(TEXT("%s: the palm is on the shaft's line"), Case.Name), OnShaft.Size() < 1.0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
