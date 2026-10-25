// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload]
// Renders an accessory as the catalog thumbnail does (FMantleCosmeticThumbnail, AccessorySubject: the engine's thumbnail scene,
// framed on the mesh), from the front and angled, into <Directory>/<Label>_{front,angled}.png. With "upload" it draws what the
// export uploads instead of the source: the reduced copy when the mesh is over the triangle limit, centred on the socket when
// its pivot is out of the slot's reach (FMantleAccessorySettings). Nothing is saved or uploaded. Logs MANTLE_RENDER_ACCESSORY lines.
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticThumbnail.h"
#include "MantlePng.h"

#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleRenderAccessory, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevRenderAccessoryPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleRenderAccessory, Error, TEXT("MANTLE_RENDER_ACCESSORY usage: Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload]"));
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
		UE_LOG(LogMantleRenderAccessory, Display, TEXT("MANTLE_RENDER_ACCESSORY done"));
	}

	FAutoConsoleCommand GMantleDevRenderAccessoryCommand(
		TEXT("Mantle.Dev.RenderAccessory"),
		TEXT("Mantle.Dev.RenderAccessory <StaticMesh> <Directory> <Label> [slot] [size] [upload] -- renders an accessory as the catalog thumbnail does, front and angled, ")
		TEXT("into Directory; with upload, as the export would upload it (reduced, centred on the socket). Saves and uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
