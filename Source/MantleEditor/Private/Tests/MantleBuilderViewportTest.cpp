// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleBuilderViewport.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleBuilderViewportTestPrivate
{
	static FMantleBuilderPointRole MakeRole(bool bRotates, bool bYawOnly)
	{
		FMantleBuilderPointRole Role;
		Role.Role = "Test";
		Role.bRotates = bRotates;
		Role.bYawOnly = bYawOnly;
		return Role;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportTranslateTest, "Mantle.Editor.BuilderViewport.TranslateRootPoint", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderViewportTranslateTest::RunTest(const FString& Parameters)
{
	using namespace MantleBuilderViewportTestPrivate;
	const FTransform Current(FQuat::Identity, FVector(10, 20, 30));
	const FTransform Result = MantleBuilderViewportMath::ApplyDrag(MakeRole(true, false), FTransform::Identity, Current, FVector(1, 2, 3), FRotator::ZeroRotator);
	TestTrue(TEXT("the point moves by the world delta"), Result.GetLocation().Equals(FVector(11, 22, 33), 0.001));
	TestTrue(TEXT("the rotation is unchanged"), Result.GetRotation().Equals(FQuat::Identity, 0.0001));

	// A parent turned 90 degrees about up: the local point (0, 5, 0) sits at world (95, 0, 0), and a world +X move of 10 puts it at (105, 0, 0).
	const FTransform Parent(FRotator(0, 90, 0), FVector(100, 0, 0));
	const FTransform InParent = MantleBuilderViewportMath::ApplyDrag(MakeRole(true, false), Parent, FTransform(FVector(0, 5, 0)), FVector(10, 0, 0), FRotator::ZeroRotator);
	TestTrue(TEXT("a parented point lands where the world delta puts it"), (InParent * Parent).GetLocation().Equals(FVector(105, 0, 0), 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportYawOnlyTest, "Mantle.Editor.BuilderViewport.YawOnlyDropsPitchAndRoll", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderViewportYawOnlyTest::RunTest(const FString& Parameters)
{
	using namespace MantleBuilderViewportTestPrivate;
	const FTransform Result = MantleBuilderViewportMath::ApplyDrag(MakeRole(true, true), FTransform::Identity, FTransform::Identity, FVector::ZeroVector, FRotator(10, 30, 20));
	const FRotator Rotation = Result.GetRotation().Rotator();
	TestTrue(TEXT("the yaw is kept"), FMath::IsNearlyEqual(Rotation.Yaw, 30.0, 0.01));
	TestTrue(TEXT("pitch is dropped"), FMath::IsNearlyZero(Rotation.Pitch, 0.01));
	TestTrue(TEXT("roll is dropped"), FMath::IsNearlyZero(Rotation.Roll, 0.01));

	const FTransform Free = MantleBuilderViewportMath::ApplyDrag(MakeRole(true, false), FTransform::Identity, FTransform::Identity, FVector::ZeroVector, FRotator(10, 30, 20));
	TestTrue(TEXT("a free role keeps pitch"), FMath::IsNearlyEqual(Free.GetRotation().Rotator().Pitch, 10.0, 0.01));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportNoRotationTest, "Mantle.Editor.BuilderViewport.NonRotatingRoleIgnoresRotation", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderViewportNoRotationTest::RunTest(const FString& Parameters)
{
	using namespace MantleBuilderViewportTestPrivate;
	const FTransform Current(FRotator(0, 15, 0), FVector(1, 2, 3));
	const FTransform Result = MantleBuilderViewportMath::ApplyDrag(MakeRole(false, false), FTransform::Identity, Current, FVector(1, 0, 0), FRotator(40, 50, 60));
	TestTrue(TEXT("the rotation stays"), Result.GetRotation().Equals(Current.GetRotation(), 0.0001));
	TestTrue(TEXT("the translation still applies"), Result.GetLocation().Equals(FVector(2, 2, 3), 0.001));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportParentTest, "Mantle.Editor.BuilderViewport.ResolveParent", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderViewportParentTest::RunTest(const FString& Parameters)
{
	FTransform Out;
	TestTrue(TEXT("no parent is the root"), MantleBuilderViewportMath::ResolveParent(nullptr, NAME_None, Out));
	TestTrue(TEXT("the root is the identity"), Out.Equals(FTransform::Identity));
	TestFalse(TEXT("an unknown parent is refused"), MantleBuilderViewportMath::ResolveParent(nullptr, "NoSuchBone", Out));
	TestTrue(TEXT("and falls back to the root"), Out.Equals(FTransform::Identity));

	// A static mesh socket (a transient copy of the engine cube, so the asset stays untouched).
	if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
	{
		UStaticMesh* Copy = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage());
		UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Copy);
		Socket->SocketName = "Muzzle";
		Socket->RelativeLocation = FVector(5, 6, 7);
		Socket->RelativeRotation = FRotator(0, 90, 0);
		Socket->RelativeScale = FVector::OneVector;
		Copy->AddSocket(Socket);
		TestTrue(TEXT("a static mesh socket resolves"), MantleBuilderViewportMath::ResolveParent(Copy, "Muzzle", Out));
		TestTrue(TEXT("at the socket's place"), Out.GetLocation().Equals(FVector(5, 6, 7), 0.001));
		TestFalse(TEXT("a static mesh has no bones"), MantleBuilderViewportMath::ResolveParent(Copy, "NoSuchBone", Out));
	}
	else
	{
		AddWarning(TEXT("/Engine/BasicShapes/Cube not found; the static mesh socket case was skipped."));
	}

	// A skeletal mesh bone and socket (a transient copy of the engine's skeletal cube).
	if (USkeletalMesh* Cube = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube")))
	{
		USkeletalMesh* Copy = DuplicateObject<USkeletalMesh>(Cube, GetTransientPackage());
		const FReferenceSkeleton& Ref = Copy->GetRefSkeleton();
		if (Ref.GetNum() > 0)
		{
			const FName RootBone = Ref.GetBoneName(0);
			TestTrue(TEXT("a bone resolves"), MantleBuilderViewportMath::ResolveParent(Copy, RootBone, Out));
			TestTrue(TEXT("to its reference pose"), Out.Equals(Ref.GetRefBonePose()[0], 0.001));

			USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Copy);
			Socket->SocketName = "Hand";
			Socket->BoneName = RootBone;
			Socket->RelativeLocation = FVector(1, 2, 3);
			Copy->AddSocket(Socket);
			TestTrue(TEXT("a socket resolves"), MantleBuilderViewportMath::ResolveParent(Copy, "Hand", Out));
			const FTransform Expected = Socket->GetSocketLocalTransform() * Ref.GetRefBonePose()[0];
			TestTrue(TEXT("through its bone"), Out.Equals(Expected, 0.001));
		}
		TestFalse(TEXT("an unknown name on a skeletal mesh is refused"), MantleBuilderViewportMath::ResolveParent(Copy, "NoSuchBone", Out));
	}
	else
	{
		AddWarning(TEXT("/Engine/EngineMeshes/SkeletalCube not found; the bone and socket cases were skipped."));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportPickTest, "Mantle.Editor.BuilderViewport.PickHandle", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FMantleBuilderViewportPickTest::RunTest(const FString& Parameters)
{
	using MantleBuilderViewportMath::FScreenHandle;
	TArray<FScreenHandle> Handles;
	Handles.Add({FName("Grip_R"), FVector2D(100, 100)});
	Handles.Add({FName("Grip_L"), FVector2D(112, 100)});
	Handles.Add({FName("Muzzle"), FVector2D(400, 250)});

	TestEqual(TEXT("a click on a handle picks it"), MantleBuilderViewportMath::PickHandle(Handles, FVector2D(400, 250)), FName("Muzzle"));
	TestEqual(TEXT("a click a few pixels off still picks it"), MantleBuilderViewportMath::PickHandle(Handles, FVector2D(409, 244)), FName("Muzzle"));
	TestEqual(TEXT("with two close, the nearer one wins"), MantleBuilderViewportMath::PickHandle(Handles, FVector2D(108, 100)), FName("Grip_L"));
	TestEqual(TEXT("empty space picks nothing"), MantleBuilderViewportMath::PickHandle(Handles, FVector2D(250, 400)), NAME_None);
	TestEqual(TEXT("just outside the tolerance picks nothing"), MantleBuilderViewportMath::PickHandle(Handles, FVector2D(400 + MantleBuilderViewportMath::PickTolerancePx + 1.0, 250)), NAME_None);
	TestEqual(TEXT("no handles picks nothing"), MantleBuilderViewportMath::PickHandle(TArray<FScreenHandle>(), FVector2D(0, 0)), NAME_None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMantleBuilderViewportRayOnPlaneTest, "Mantle.Editor.BuilderViewport.RayOnPlane", EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBuilderViewportRayOnPlaneTest::RunTest(const FString& Parameters)
{
	// A camera 100 cm back looking down +X at a point on the plane x = 0: the ray through a cursor 20 cm up and 10 cm right
	// lands there, so a dragged point follows the cursor.
	FVector Hit;
	TestTrue(TEXT("a ray toward the plane meets it"), MantleBuilderViewportMath::RayOnPlane(FVector(-100, 0, 0), FVector(100, 10, 20).GetSafeNormal(), FVector::ZeroVector, FVector::ForwardVector, Hit));
	TestTrue(TEXT("where the cursor points"), Hit.Equals(FVector(0, 10, 20), 1.0e-6));
	TestFalse(TEXT("a ray along the plane misses"), MantleBuilderViewportMath::RayOnPlane(FVector(-100, 0, 0), FVector::RightVector, FVector::ZeroVector, FVector::ForwardVector, Hit));
	TestFalse(TEXT("a ray pointing away misses"), MantleBuilderViewportMath::RayOnPlane(FVector(-100, 0, 0), -FVector::ForwardVector, FVector::ZeroVector, FVector::ForwardVector, Hit));
	return true;
}

#endif
