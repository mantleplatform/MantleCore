// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload] [body]
// Renders an accessory as the catalog thumbnail does (FMantleCosmeticThumbnail, AccessorySubject: the engine's thumbnail scene,
// framed on the mesh), from the front and angled, into <Directory>/<Label>_{front,angled}.png. With "upload" it draws what the
// export uploads instead of the source: the reduced copy when the mesh is over the triangle limit, centerd on the socket when
// its pivot is out of the slot's reach (FMantleAccessorySettings). Nothing is saved or uploaded. Logs MANTLE_RENDER_ACCESSORY lines.
#include "MantleAccessorySettings.h"
#include "MantleAccessoryPlacement.h"
#include "MantleAvatarContract.h"
#include "MantleEditorActions.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleFitBodies.h"
#include "MantlePng.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRenderAccessory, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRenderAccessoryPrivate
{
	/**
	 * The accessory on one of Epic's standard bodies (made in memory, as the garment fits are) in the reference pose, on the slot's
	 * bone at the body's own socket when it has one, in the catalog thumbnail's scene. Placed is the upload's recentring.
	 */
	void RunWorn(UStaticMesh* Shown, const FTransform& Placed, const FTransform& Attach, const FMantleAvatarContract& Contract, const FString& Slot, const FString& BodyId, const FString& Directory,
		const FString& Label, const int32 Size)
	{
		EMetaHumanBodyType Type;
		FMantleFitBody Body;
		FString Error;
		const FMantleContractSlot* Point = Contract.Slots.FindByPredicate([&Slot](const FMantleContractSlot& Entry) { return Entry.Id == Slot && Entry.Group == TEXT("accessory"); });
		if (!Point || !FMantleFitBodies::ParseBodyType(BodyId, Type) || !FMantleFitBodies::Make(BodyId, Type, Body, Error))
		{
			UE_LOG(LogMantleRenderAccessory, Error, TEXT("MANTLE_RENDER_ACCESSORY worn FAILED: %s"), Point ? (Error.IsEmpty() ? TEXT("unknown body type") : *Error) : TEXT("not an accessory slot"));
			return;
		}
		const FReferenceSkeleton& Skeleton = Body.BodyMesh->GetRefSkeleton();
		const int32 Bone = Skeleton.FindRawBoneIndex(FName(*Point->SocketBone));
		if (Bone == INDEX_NONE)
		{
			UE_LOG(LogMantleRenderAccessory, Error, TEXT("MANTLE_RENDER_ACCESSORY worn FAILED: %s has no %s bone"), *BodyId, *Point->SocketBone);
			return;
		}
		FTransform Pose = FTransform::Identity;
		for (int32 Index = Bone; Index != INDEX_NONE; Index = Skeleton.GetRawParentIndex(Index)) { Pose = Pose * Skeleton.GetRawRefBonePose()[Index]; }
		const USkeletalMeshSocket* Own = Body.BodyMesh->FindSocket(FName(*Slot));
		const FTransform Socket = Own && Own->BoneName == FName(*Point->SocketBone) ? Own->GetSocketLocalTransform() * Pose : Pose;
		const FTransform Worn = Placed * Attach * Socket;
		for (const TCHAR* View : { TEXT("front"), TEXT("back"), TEXT("side") })
		{
			// The body and the face as the editor evaluated them (the merged head+body has no render data of its own here).
			FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::GarmentSubject(Body.BodyMesh.Get(), Body.FaceMesh.Get(), nullptr, nullptr, Shown->GetBoundingBox().TransformBy(Worn));
			Shot.bFloor = true;
			Shot.StaticMesh = Shown;
			Shot.StaticTransform = Worn;
			Shot.Describe = FString::Printf(TEXT("%s worn on %s"), *Shown->GetName(), *BodyId);
			if (FCString::Strcmp(View, TEXT("back")) == 0) { Shot.View.Yaw += 180.0f; }
			if (FCString::Strcmp(View, TEXT("side")) == 0) { Shot.View.Yaw += 90.0f; }
			TArray<FColor> Pixels;
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_worn_%s.png"), *Label, View));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
				&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY worn %s %s %s"), View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleRenderAccessory, Error, TEXT("MANTLE_RENDER_ACCESSORY usage: Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload] [body]"));
			return;
		}
		FString Path = Args[0];
		if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, *Path);
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		FString WhyNot;
		if (!Source || !Contract || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleRenderAccessory, Error, TEXT("MANTLE_RENDER_ACCESSORY failed: %s is not a static mesh, there is no contract, or nothing can render (%s)"), *Path, *WhyNot);
			return;
		}
		const FString Slot = Args.Num() > 3 ? Args[3] : FString(TEXT("acc_neck"));
		const int32 Size = Args.Num() > 4 ? FMath::Clamp(FCString::Atoi(*Args[4]), 128, 2048) : 1024;
		const bool bUpload = Args.Num() > 5 && Args[5] == TEXT("upload");
		const FString BodyArg = Args.Num() > 6 ? Args[6] : FString();
		const bool bBody = BodyArg == TEXT("body") || BodyArg.StartsWith(TEXT("body:"));

		UStaticMesh* Shown = Source;
		FTransform Placed = FTransform::Identity;
		if (bUpload)
		{
			Shown = const_cast<UStaticMesh*>(FMantleAccessorySettings::UploadMesh(Source, Contract->RigidAccessory));
			if (const FMantleContractAccessoryBounds* Limits = Contract->AccessoryBounds.Find(Slot))
			{
				Placed = FTransform(FVector(-FMantleAccessorySettings::RecenterOffset(FBox3f(Shown->GetBoundingBox()), *Limits)));
			}
		}
		// The placement the panel and the upload use for this mesh (saved, else aligned to the avatar's axes).
		FTransform Attach = FTransform::Identity;
		if (bBody)
		{
			const FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, Source, Slot, FString(), *Contract, EMantleFitSex::Auto);
			if (Draft.bAttach)
			{
				FMantleCosmeticTransform Placement;
				Placement.Rotation = FRotator(Draft.AttachRotation.X, Draft.AttachRotation.Y, Draft.AttachRotation.Z);
				Placement.Location = FVector(Draft.AttachLocation);
				Placement.Scale = FVector(Draft.AttachScale);
				Attach = MantleAccessoryPlacement::ClampedAttach(Placement);
			}
			UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY placement attach=%d rotation=%s"), Draft.bAttach, *Draft.AttachRotation.ToString());
		}
		UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY %s bounds=%s moved=%s"), *Shown->GetName(), *Shown->GetBoundingBox().ToString(), *Placed.GetLocation().ToString());

		for (const TCHAR* View : { TEXT("front"), TEXT("angled") })
		{
			FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::AccessorySubject(Shown);
			Shot.StaticTransform = Placed;
			Shot.Frame = Shown->GetBoundingBox().TransformBy(Placed);
			if (FCString::Strcmp(View, TEXT("front")) == 0)
			{
				Shot.View = FRotator(0.0f, 180.0f, 0.0f);
			}
			TArray<FColor> Pixels;
			FString Error;
			const FString File = FPaths::Combine(Args[1], FString::Printf(TEXT("%s_%s.png"), *Args[2], View));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
				&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY %s %s %s"), View, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
		if (bBody)
		{
			RunWorn(Shown, Placed, Attach, *Contract, Slot, BodyArg.StartsWith(TEXT("body:")) ? BodyArg.Mid(5) : FString(TEXT("f_med_nrw")), Args[1], Args[2], Size);
		}
		UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY done"));
	}

	FAutoConsoleCommand GMantleDevRenderAccessoryCommand(
		TEXT("Mantle.Dev.RenderAccessory"),
		TEXT("Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload] [body|body:<type>] -- renders an accessory as the catalog thumbnail does, front and angled, ")
		TEXT("into Directory; with upload, as the export would upload it (reduced, centerd on the socket); with body, also worn on a standard body. Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
