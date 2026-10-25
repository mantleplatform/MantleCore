// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeadFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Hats fit every head: the reference cranium maps onto a measured one, and the acc_hat socket carries that map. Maths only.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHeadFitMapsReferenceOntoHeadTest,
	"Mantle.Editor.HeadFit.MapsReferenceOntoHead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHeadFitMapsReferenceOntoHeadTest::RunTest(const FString& Parameters)
{
	// The reference head as measured (MH_MantleNeutral) and a head 20% smaller (ROXY measured 0.82).
	const FBox3f Reference(FVector3f(0.03f, -7.66f, -8.92f), FVector3f(16.65f, 13.01f, 8.90f));
	const FBox3f Small(Reference.Min * 0.8f, Reference.Max * 0.8f);
	const FMantleHeadFit::FFit Fit = FMantleHeadFit::FitTo(Reference, Small);
	TestTrue(TEXT("the scale is the size ratio on every axis"), Fit.Scale.Equals(FVector3f(0.8f), 1e-4f));
	for (const FVector3f& Corner : { Reference.Min, Reference.Max, Reference.GetCenter() })
	{
		const FVector3f Mapped = Fit.Scale * Corner + Fit.Offset;
		const FVector3f Want = Corner * 0.8f;
		TestTrue(FString::Printf(TEXT("%s lands on %s"), *Corner.ToString(), *Want.ToString()), Mapped.Equals(Want, 1e-3f));
	}

	// The same head is identity.
	const FMantleHeadFit::FFit Same = FMantleHeadFit::FitTo(Reference, Reference);
	TestTrue(TEXT("the reference onto itself scales by 1"), Same.Scale.Equals(FVector3f::OneVector, 1e-5f));
	TestTrue(TEXT("the reference onto itself moves nothing"), Same.Offset.Equals(FVector3f::ZeroVector, 1e-4f));

	// A head past the socket's bounds is clamped to them and stays centred.
	const FBox3f Huge(Reference.Min * 3.0f, Reference.Max * 3.0f);
	const FMantleHeadFit::FFit Clamped = FMantleHeadFit::FitTo(Reference, Huge);
	TestTrue(TEXT("the scale is clamped to 2"), Clamped.Scale.Equals(FVector3f(2.0f), 1e-5f));
	TestTrue(TEXT("the clamped map keeps the centres together"),
		(Clamped.Scale * Reference.GetCenter() + Clamped.Offset).Equals(Huge.GetCenter(), 1e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHeadFitHatSocketTest,
	"Mantle.Editor.HeadFit.HatSocketCarriesTheMap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHeadFitHatSocketTest::RunTest(const FString& Parameters)
{
	const FBox3f Reference(FVector3f(0.0f, -8.0f, -9.0f), FVector3f(16.0f, 12.0f, 9.0f));
	const FBox3f Measured(FVector3f(1.0f, -7.0f, -8.0f), FVector3f(15.0f, 11.0f, 8.0f));
	const FMantleHeadFit::FFit Fit = FMantleHeadFit::FitTo(Reference, Measured);

	// Mantle's sockets sit at identity on the head: the override is the map itself.
	const FMantleCosmeticSocketOverride Identity = FMantleHeadFit::HatSocket(Reference, Measured, FTransform::Identity);
	TestEqual(TEXT("the override is for acc_hat"), Identity.Socket, FString(TEXT("acc_hat")));
	TestTrue(TEXT("location and scale are on, rotation is the socket's"), Identity.bLocation && Identity.bScale && !Identity.bRotation);
	TestTrue(TEXT("the location is the offset"), Identity.Location.Equals(Fit.Offset, 1e-4f));
	TestTrue(TEXT("the scale is the fit's"), Identity.Scale.Equals(Fit.Scale, 1e-5f));

	// A socket turned and moved on the head: a hat point lands where the map, in the socket's frame, puts it.
	const FTransform Socket(FRotator(0.0f, 90.0f, 0.0f), FVector(2.0f, 3.0f, 4.0f));
	const FMantleCosmeticSocketOverride Turned = FMantleHeadFit::HatSocket(Reference, Measured, Socket);
	const FTransform Overridden(Socket.GetRotation(), FVector(Turned.Location), FVector(Turned.Scale));
	const FVector3f HatPoint(16.0f, 0.0f, 0.0f);
	const FVector Got = Overridden.TransformPosition(FVector(HatPoint));
	const FVector Want = Socket.TransformPosition(FVector(Fit.Scale * HatPoint + Fit.Offset));
	TestTrue(FString::Printf(TEXT("a hat point lands at %s (got %s)"), *Want.ToString(), *Got.ToString()), Got.Equals(Want, 1e-3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHeadFitIgnoresEarsTest,
	"Mantle.Editor.HeadFit.IgnoresEars",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHeadFitIgnoresEarsTest::RunTest(const FString& Parameters)
{
	// MetaHuman's ear bones, on both sides, are ears (MetaGoblin's ears made its hat 1.57 times too wide).
	for (const TCHAR* Bone : { TEXT("FACIAL_L_Ear"), TEXT("FACIAL_R_Ear"), TEXT("FACIAL_L_Ear1"), TEXT("FACIAL_R_Ear4"), TEXT("facial_l_ear2") })
	{
		TestTrue(FString::Printf(TEXT("%s is an ear"), Bone), FMantleHeadFit::IsEarBone(FName(Bone)));
	}
	// Names that only contain the letters are not.
	for (const TCHAR* Bone : { TEXT("head"), TEXT("FACIAL_C_Forehead"), TEXT("FACIAL_L_ForeheadMid"), TEXT("FACIAL_L_Earring"), TEXT("FACIAL_L_Nearside"),
		TEXT("FACIAL_R_EyesackUpper"), TEXT("FACIAL_C_Skull") })
	{
		TestFalse(FString::Printf(TEXT("%s is not an ear"), Bone), FMantleHeadFit::IsEarBone(FName(Bone)));
	}

	// A vertex is cranium when it is mostly on the head and barely on an ear.
	TestTrue(TEXT("all on the head, none on an ear: cranium"), FMantleHeadFit::IsCranium(1.0f, 0.0f));
	TestTrue(TEXT("a trace of ear weight (a seam by the ear) still counts"), FMantleHeadFit::IsCranium(1.0f, 0.02f));
	TestFalse(TEXT("an ear vertex does not count"), FMantleHeadFit::IsCranium(1.0f, 1.0f));
	TestFalse(TEXT("a vertex shared with an ear does not count"), FMantleHeadFit::IsCranium(1.0f, 0.3f));
	TestFalse(TEXT("a neck vertex does not count"), FMantleHeadFit::IsCranium(0.3f, 0.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHeadFitFrontIsForeheadTest,
	"Mantle.Editor.HeadFit.FrontIsForehead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHeadFitFrontIsForeheadTest::RunTest(const FString& Parameters)
{
	// A skull: the upper half of a 10 cm ball on the joint (X up, Y front-back with +Y the face, Z side to side).
	TArray<FVector3f> Skull;
	for (int32 Up = 0; Up <= 20; ++Up)
	{
		for (int32 Around = 0; Around < 36; ++Around)
		{
			const float Polar = UE_HALF_PI * Up / 20.0f;
			const float Azimuth = UE_TWO_PI * Around / 36.0f;
			Skull.Add(10.0f * FVector3f(FMath::Cos(Polar), FMath::Sin(Polar) * FMath::Cos(Azimuth), FMath::Sin(Polar) * FMath::Sin(Azimuth)));
		}
	}
	const FBox3f Plain = FMantleHeadFit::CraniumBox(Skull);
	TestTrue(FString::Printf(TEXT("the plain skull is 10 cm tall (%s)"), *Plain.ToString()), FMath::IsNearlyEqual(Plain.Max.X, 10.0f, 0.05f));

	// The same skull with a face below the forehead: a nose 6 cm in front of it, 2 cm up.
	TArray<FVector3f> WithNose = Skull;
	for (int32 Index = 0; Index < 200; ++Index)
	{
		WithNose.Add(FVector3f(2.0f + Index * 0.01f, 16.0f, 0.0f));
	}
	const FBox3f Box = FMantleHeadFit::CraniumBox(WithNose);
	TestTrue(FString::Printf(TEXT("the nose does not move the front (%.2f, without it %.2f)"), Box.Max.Y, Plain.Max.Y),
		FMath::IsNearlyEqual(Box.Max.Y, Plain.Max.Y, 0.05f));
	TestTrue(TEXT("the front is the forehead: the ball's front at 55% of its height"),
		FMath::IsNearlyEqual(Box.Max.Y, 10.0f * FMath::Sqrt(1.0f - 0.55f * 0.55f), 0.6f));
	TestTrue(TEXT("the back is the skull's own"), FMath::IsNearlyEqual(Box.Min.Y, Plain.Min.Y, 0.05f));
	TestTrue(TEXT("the width is the skull's own"), FMath::IsNearlyEqual(Box.GetSize().Z, Plain.GetSize().Z, 0.05f));

	TestFalse(TEXT("no points, no box"), static_cast<bool>(FMantleHeadFit::CraniumBox({}).IsValid));
	return true;
}

#endif
