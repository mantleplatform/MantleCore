// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.VehicleOpenShot <VehicleSource> <Directory> [size] [degrees]
// Draws a built vehicle as its library card does (FMantleVehicleThumbnail: the engine's thumbnail scene through
// FMantleCosmeticThumbnail), once shut and once with every hinge it has turned open: the doors, the hood and the trunk.
// Each hinge turns its bone about its pivot and axis by degrees (default 70) times its scale, as the game turns it.
// Writes <Directory>/<Name>_shut_front.png, _open_front.png, _shut_back.png and _open_back.png. Nothing is saved or uploaded.
// Logs MANTLE_VEHICLE_OPEN_SHOT lines.
#include "MantleCosmeticThumbnail.h"
#include "MantlePng.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleThumbnail.h"

#include "AnimationRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleVehicleOpenShot, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevVehicleOpenShotPrivate
{
	/** Every hinge the vehicle has on, with what it is called for the log. */
	TArray<TPair<FString, FMantleVehicleSourceHinge>> Hinges(const FMantleVehicleSourceTuning& T)
	{
		TArray<TPair<FString, FMantleVehicleSourceHinge>> Out;
		const auto Add = [&Out](const bool bOn, const TCHAR* What, const FMantleVehicleSourceHinge& Hinge)
		{
			if (bOn && !Hinge.Bone.IsNone())
			{
				Out.Emplace(What, Hinge);
			}
		};
		Add(T.bHasDriverDoor, TEXT("driver door"), T.DriverDoor);
		Add(T.bPassengerSeat && T.bHasPassengerDoor, TEXT("passenger door"), T.PassengerDoor);
		Add(T.bRearLeftDoor, TEXT("rear left door"), T.RearLeftDoor);
		Add(T.bRearRightDoor, TEXT("rear right door"), T.RearRightDoor);
		Add(T.bHood, TEXT("hood"), T.HoodLid);
		Add(T.bTrunk, TEXT("trunk"), T.TrunkLid);
		return Out;
	}

	/** Mesh's reference pose with each hinge's bone turned about its pivot, in component space (children ride their hinge). */
	TArray<FTransform> OpenPose(const USkeletalMesh& Mesh, const TArray<TPair<FString, FMantleVehicleSourceHinge>>& Open, const double Degrees)
	{
		const FReferenceSkeleton& Skeleton = Mesh.GetRefSkeleton();
		TArray<FTransform> Local = Skeleton.GetRefBonePose();
		TArray<FTransform> Component;
		FAnimationRuntime::FillUpComponentSpaceTransforms(Skeleton, Local, Component);
		for (const TPair<FString, FMantleVehicleSourceHinge>& Each : Open)
		{
			const FMantleVehicleSourceHinge& Hinge = Each.Value;
			const int32 Bone = Skeleton.FindBoneIndex(Hinge.Bone);
			if (Bone <= 0 || Hinge.Axis.IsNearlyZero())
			{
				UE_LOG(LogMantleVehicleOpenShot, Warning, TEXT("MANTLE_VEHICLE_OPEN_SHOT the %s's bone %s is not in the body"), *Each.Key, *Hinge.Bone.ToString());
				continue;
			}
			const FQuat Turn(Hinge.Axis.GetSafeNormal(), FMath::DegreesToRadians(Degrees * Hinge.Scale));
			const FTransform& Rest = Component[Bone];
			const FTransform Turned(Turn * Rest.GetRotation(), Hinge.Pivot + Turn.RotateVector(Rest.GetLocation() - Hinge.Pivot), Rest.GetScale3D());
			const int32 Parent = Skeleton.GetParentIndex(Bone);
			Local[Bone] = Turned.GetRelativeTransform(Component[Parent]);
			UE_LOG(LogMantleVehicleOpenShot, Display, TEXT("MANTLE_VEHICLE_OPEN_SHOT %s: bone %s about %s along %s by %.0f degrees"), *Each.Key, *Hinge.Bone.ToString(),
				*Hinge.Pivot.ToString(), *Hinge.Axis.ToString(), Degrees * Hinge.Scale);
		}
		FAnimationRuntime::FillUpComponentSpaceTransforms(Skeleton, Local, Component);
		return Component;
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantleVehicleOpenShot, Error, TEXT("MANTLE_VEHICLE_OPEN_SHOT usage: Mantle.Dev.VehicleOpenShot <VehicleSource> <Directory> [size] [degrees]"));
			return;
		}
		const int32 Size = Args.Num() > 2 ? FMath::Clamp(FCString::Atoi(*Args[2]), 128, 2048) : 1024;
		const double Degrees = Args.Num() > 3 ? FCString::Atod(*Args[3]) : 70.0;
		const UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Args[0]);
		USkeletalMesh* Mesh = Source ? Source->Body.LoadSynchronous() : nullptr;
		FString WhyNot;
		if (!Mesh || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleVehicleOpenShot, Error, TEXT("MANTLE_VEHICLE_OPEN_SHOT failed: no vehicle or body at %s, or nothing can render (%s)"), *Args[0], *WhyNot);
			return;
		}
		const TArray<TPair<FString, FMantleVehicleSourceHinge>> Open = Hinges(Source->Tuning);
		UE_LOG(LogMantleVehicleOpenShot, Display, TEXT("MANTLE_VEHICLE_OPEN_SHOT %s has %d hinges"), *Source->GetName(), Open.Num());
		const TArray<FTransform> Pose = OpenPose(*Mesh, Open, Degrees);

		// Framed on the body with room for what swings out of it.
		const FBox Bounds = Mesh->GetBounds().GetBox();
		const FBox Frame = Bounds.ExpandBy(FVector(Bounds.GetExtent().X * 0.25, Bounds.GetExtent().Y * 0.6, Bounds.GetExtent().Z * 0.4));
		const FRotator Front = FMantleVehicleThumbnail::View();
		const FRotator Back(Front.Pitch, Front.Yaw + 180.0, 0.0);
		for (const bool bOpen : { false, true })
		{
			for (const TPair<const TCHAR*, FRotator>& View : { TPair<const TCHAR*, FRotator>(TEXT("front"), Front), TPair<const TCHAR*, FRotator>(TEXT("back"), Back) })
			{
				FMantleThumbnailSubject Shot;
				Shot.SkeletalMeshes.Add(Mesh);
				Shot.Frame = Frame;
				Shot.View = View.Value;
				Shot.Describe = FString::Printf(TEXT("%s, %s, %s"), *Mesh->GetName(), bOpen ? TEXT("open") : TEXT("shut"), View.Key);
				if (bOpen)
				{
					Shot.Pose = [&Pose](FPreviewScene&, TConstArrayView<USkeletalMeshComponent*> Components, FBox&)
					{
						USkeletalMeshComponent* Body = Components.Num() > 0 ? Components[0] : nullptr;
						if (Body && Body->GetEditableComponentSpaceTransforms().Num() == Pose.Num())
						{
							// Written straight into the pose the component draws (the editor's own bone tools do the same); it plays no animation to undo it.
							Body->bNoSkeletonUpdate = true;
							Body->GetEditableComponentSpaceTransforms() = Pose;
							Body->ApplyEditedComponentSpaceTransforms();
						}
					};
				}
				TArray<FColor> Pixels;
				FString Error;
				const FString File = FPaths::Combine(Args[1], FString::Printf(TEXT("%s_%s_%s.png"), *Source->GetName(), bOpen ? TEXT("open") : TEXT("shut"), View.Key));
				const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
					&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
				UE_LOG(LogMantleVehicleOpenShot, Display, TEXT("MANTLE_VEHICLE_OPEN_SHOT %s %s"), bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
			}
		}
		UE_LOG(LogMantleVehicleOpenShot, Display, TEXT("MANTLE_VEHICLE_OPEN_SHOT done"));
	}

	FAutoConsoleCommand GMantleDevVehicleOpenShotCommand(
		TEXT("Mantle.Dev.VehicleOpenShot"),
		TEXT("Mantle.Dev.VehicleOpenShot <VehicleSource> <Directory> [size] [degrees] -- draws a built vehicle as its library card does, shut and with its doors, hood and trunk open"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
