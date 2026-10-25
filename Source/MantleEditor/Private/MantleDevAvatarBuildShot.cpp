// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.AvatarBuildShot <Character> <Directory> <Label> [default female|default male] [size]
// Draws a MetaHuman Character's preview body and face as the catalog thumbnail does (FMantleCosmeticThumbnail: the engine's thumbnail
// scene, reference pose), the whole standing body from the front and the side, into <Directory>/<Label>_{front,side}.png. With "default
// female" or "default male" it draws a fresh character instead (MetaHuman's own body at Mantle's default height, no face conform) to put
// beside a kept face's seeded sliders (#50). Logs the body's Weight, Fat and Muscularity sliders. Nothing is saved or uploaded.
// Logs MANTLE_AVATAR_BUILD_SHOT lines.
#include "MantleAvatarHeight.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleMetaHumanBuild.h"
#include "MantleMetaHumanConform.h"
#include "MantlePng.h"

#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterFactoryNew.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleAvatarBuildShot, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevAvatarBuildShotPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleAvatarBuildShot, Error, TEXT("MANTLE_AVATAR_BUILD_SHOT usage: Mantle.Dev.AvatarBuildShot <Character> <Directory> <Label> [default female|default male] [size]"));
			return;
		}
		const bool bDefault = Args.Num() > 4 && Args[3] == TEXT("default");
		const bool bFeminine = bDefault && Args[4] == TEXT("female");
		const int32 Size = Args.Num() > (bDefault ? 5 : 3) ? FMath::Clamp(FCString::Atoi(*Args[bDefault ? 5 : 3]), 128, 2048) : 1024;
		UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
		FString WhyNot;
		if (!Subsystem || !FMantleCosmeticThumbnail::CanCapture(&WhyNot))
		{
			UE_LOG(LogMantleAvatarBuildShot, Error, TEXT("MANTLE_AVATAR_BUILD_SHOT failed: MetaHuman Creator is not running, or nothing can render (%s)"), *WhyNot);
			return;
		}
		UMetaHumanCharacter* Character = nullptr;
		FString Error;
		if (bDefault)
		{
			UMetaHumanCharacterFactoryNew* Factory = NewObject<UMetaHumanCharacterFactoryNew>(GetTransientPackage());
			Character = Cast<UMetaHumanCharacter>(Factory->FactoryCreateNew(UMetaHumanCharacter::StaticClass(), GetTransientPackage(), TEXT("DefaultBuildShot"),
				RF_Transient, nullptr, GWarn));
		}
		else
		{
			FString Path = Args[0];
			if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
			{
				Path += TEXT(".") + FPackageName::GetShortName(Path);
			}
			Character = LoadObject<UMetaHumanCharacter>(nullptr, *Path);
		}
		if (!Character || !Subsystem->TryAddObjectToEdit(Character))
		{
			UE_LOG(LogMantleAvatarBuildShot, Error, TEXT("MANTLE_AVATAR_BUILD_SHOT failed: the character %s could not be opened for editing"), *Args[0]);
			return;
		}
		ON_SCOPE_EXIT
		{
			if (Subsystem->IsObjectAddedForEditing(Character))
			{
				Subsystem->RemoveObjectToEdit(Character);
			}
		};
		if (bDefault)
		{
			bool bFixed = false;
			if (!FMantleMetaHumanBuild::SetHeight(*Character, FMantleAvatarHeight::TargetCm(), bFixed, Error) || !FMantleMetaHumanConform::ChooseParametricBody(*Character, bFeminine, Error))
			{
				UE_LOG(LogMantleAvatarBuildShot, Error, TEXT("MANTLE_AVATAR_BUILD_SHOT failed: the default body could not be made (%s)"), *Error);
				return;
			}
		}
		for (const FMetaHumanCharacterBodyConstraint& Constraint : Subsystem->GetBodyConstraints(Character))
		{
			const FString Name = Constraint.Name.ToString();
			if (Name == TEXT("Weight") || Name == TEXT("Fat") || Name.Contains(TEXT("Muscul")) || Name.Contains(TEXT("Masculine")))
			{
				UE_LOG(LogMantleAvatarBuildShot, Display, TEXT("MANTLE_AVATAR_BUILD_SHOT %s: %s %s %.2f (range %.2f to %.2f)"), *Args[2], *Name,
					Constraint.bIsActive ? TEXT("active") : TEXT("inactive"), Constraint.TargetMeasurement, Constraint.MinMeasurement, Constraint.MaxMeasurement);
			}
		}
		FMetaHumanCharacterPreviewAssetOptions Options;
		FMetaHumanCharacterPreviewAssets Preview;
		if (!Subsystem->TryGetCharacterPreviewAssets(Character, Options, Preview) || !Preview.BodyMesh || !Preview.FaceMesh)
		{
			UE_LOG(LogMantleAvatarBuildShot, Error, TEXT("MANTLE_AVATAR_BUILD_SHOT failed: Creator did not provide the preview body and face"));
			return;
		}
		USkeletalMesh* Body = Preview.BodyMesh.Get();
		USkeletalMesh* Face = Preview.FaceMesh.Get();
		const FBox Bounds = Body->GetImportedBounds().GetBox();
		const FVector Center = Bounds.GetCenter();
		const double Top = FMath::Max(Bounds.Max.Z, 50.0);
		for (const TPair<const TCHAR*, float> View : { TPair<const TCHAR*, float>(TEXT("front"), -90.0f), TPair<const TCHAR*, float>(TEXT("side"), 0.0f) })
		{
			FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::AvatarSubject(Body, Face);
			Shot.Frame = FBox(FVector(Center.X - 60.0, Center.Y - 60.0, 0.0), FVector(Center.X + 60.0, Center.Y + 60.0, Top + 15.0));
			Shot.bFloor = true;
			// The avatar faces +Y: the front camera looks along -Y.
			Shot.View = FRotator(-4.0f, View.Value, 0.0f);
			TArray<FColor> Pixels;
			const FString File = FPaths::Combine(Args[1], FString::Printf(TEXT("%s_%s.png"), *Args[2], View.Key));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
				&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			UE_LOG(LogMantleAvatarBuildShot, Display, TEXT("MANTLE_AVATAR_BUILD_SHOT %s %s %s"), View.Key, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
		UE_LOG(LogMantleAvatarBuildShot, Display, TEXT("MANTLE_AVATAR_BUILD_SHOT done"));
	}

	FAutoConsoleCommand GMantleDevAvatarBuildShotCommand(
		TEXT("Mantle.Dev.AvatarBuildShot"),
		TEXT("Mantle.Dev.AvatarBuildShot <Character> <Directory> <Label> [default female|default male] [size] -- draws a MetaHuman Character's whole body as the catalog thumbnail does, and logs its Weight, Fat and Muscularity sliders"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
