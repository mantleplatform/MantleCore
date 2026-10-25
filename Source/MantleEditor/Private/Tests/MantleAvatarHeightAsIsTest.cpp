// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarHeight.h"
#include "MantleAvatarHeightFixer.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// As-is avatars are resized to the default height about their feet, to within the converted tolerance.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAsIsHeightMathTest,
	"Mantle.Editor.AvatarHeight.AsIsMath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAsIsHeightMathTest::RunTest(const FString& Parameters)
{
	const double Target = FMantleAvatarHeight::TargetCm();
	// Sophie as imported: 176.5 cm.
	const double K = FMantleAvatarHeightFixer::ScaleFor(176.5, Target);
	TestTrue(TEXT("Sophie's scaled height is on the target"), FMantleAvatarHeight::IsWithin(176.5 * K, Target, FMantleAvatarHeight::ConvertedToleranceCm));
	TestTrue(TEXT("a shrink"), K < 1.0);
	TestEqual(TEXT("a zero height leaves the size"), FMantleAvatarHeightFixer::ScaleFor(0.0, Target), 1.0);
	TestEqual(TEXT("no target leaves the size"), FMantleAvatarHeightFixer::ScaleFor(170.0, 0.0), 1.0);

	// Feet stay put: a point on the floor does not move, the head lands at the target above it.
	const double FeetZ = -2.0;
	const FVector3f Foot = FMantleAvatarHeightFixer::ScalePoint(FVector3f(10.f, 5.f, static_cast<float>(FeetZ)), K, FeetZ);
	TestEqual(TEXT("the sole keeps its height"), static_cast<double>(Foot.Z), FeetZ, 1.e-4);
	const FVector3f Head = FMantleAvatarHeightFixer::ScalePoint(FVector3f(0.f, 0.f, static_cast<float>(FeetZ + 176.5)), K, FeetZ);
	TestEqual(TEXT("the head is the target above the sole"), static_cast<double>(Head.Z) - FeetZ, Target, 1.e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAsIsHeightMeshTest,
	"Mantle.Editor.AvatarHeight.AsIsMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleAsIsHeightMeshTest::RunTest(const FString& Parameters)
{
	// A real mesh through the fixer's own writer: the UE4 Mannequin, resized to the default height.
	USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/UE4_Mannequin/Meshes/SKM_UE4_Mannequin.SKM_UE4_Mannequin"));
	if (!Source)
	{
		AddWarning(TEXT("SKM_UE4_Mannequin is not in this project; skipped"));
		return true;
	}
	double Height = 0.0;
	double FeetZ = 0.0;
	if (!TestTrue(TEXT("the mannequin measures"), FMantleAvatarHeightFixer::MeasureHeight(*Source, Height, FeetZ)))
	{
		return false;
	}
	const double Target = FMantleAvatarHeight::TargetCm();
	const double K = FMantleAvatarHeightFixer::ScaleFor(Height, Target);
	FString Error;
	USkeletalMesh* Scaled = FMantleAvatarHeightFixer::WriteScaled(Source, K, FeetZ, TEXT("/Game/Automation/AsIsHeight"), TEXT("SKM_AsIsHeightTest"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the scaled mesh is written (%s)"), *Error), Scaled))
	{
		return false;
	}
	double After = 0.0;
	double AfterFeet = 0.0;
	TestTrue(TEXT("the scaled mesh measures"), FMantleAvatarHeightFixer::MeasureHeight(*Scaled, After, AfterFeet));
	AddInfo(FString::Printf(TEXT("mannequin %.2f cm -> %.2f cm (scale %.4f)"), Height, After, K));
	TestTrue(TEXT("within the converted tolerance of the default height"), FMantleAvatarHeight::IsWithin(After, Target, FMantleAvatarHeight::ConvertedToleranceCm));
	TestEqual(TEXT("the feet stay where they were"), AfterFeet, FeetZ, 0.05);
	Scaled->ClearFlags(RF_Public | RF_Standalone);
	Scaled->GetPackage()->ClearDirtyFlag();
	Scaled->MarkAsGarbage();
	return true;
}

#endif
