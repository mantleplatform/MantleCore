// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.AsIsCheck <SkeletalMesh> <Directory> <Label> [anim=<AnimSequence>] [size]
// #46: draws the source mesh as it is (<Label>_source_{front,side}.png, the reference), then converts it As-is exactly as the panel's Convert does (FMantleAvatarConverter, every fix at its default,
// nothing saved), logs the fix list, what ran and the upload checks' issues, the converted avatar's depth, height, LOD 0
// section count and morph targets; then draws it as the catalog thumbnail does (FMantleCosmeticThumbnail: the engine's
// thumbnail scene), the whole standing body from the front and the side, into <Directory>/<Label>_{front,side}.png, and with
// anim= (a UEFN mannequin clip, such as M_Neutral_Stand_Idle_Loop) also posed in it as players see it (the client's
// retargeter, FMantleEmoteCheck::GarmentPoser), <Label>_<clip>_{front,side}.png. As-is must be on in the project's
// EditorPerProjectUserSettings.ini (UMantleSettings::bAllowAsIsAvatars). Logs MANTLE_ASIS_CHECK lines.
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleEmoteCheck.h"
#include "MantlePng.h"

#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshModel.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleAsIsCheck, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevAsIsCheckPrivate
{
	const TCHAR* AsIsSeverity(const EMantleCosmeticSeverity Severity)
	{
		return Severity == EMantleCosmeticSeverity::Error ? TEXT("error") : Severity == EMantleCosmeticSeverity::Warning ? TEXT("warning") : TEXT("note");
	}

	void AsIsLogOutcomes(const TCHAR* What, TConstArrayView<FMantleConvertOutcome> Outcomes)
	{
		for (const FMantleConvertOutcome& Outcome : Outcomes)
		{
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK %s %s: %s%s%s"), What, *Outcome.Key, *Outcome.Fix.Action, Outcome.Reason.IsEmpty() ? TEXT("") : TEXT(" -- "),
				*Outcome.Reason);
		}
	}

	/** The whole standing body, front and side, as the catalog draws it; Pose set: posed by PoseAt at Time. */
	void AsIsShots(USkeletalMesh* Body, const FString& Directory, const FString& Label, const int32 Size,
		const TFunction<void(FMantleThumbnailSubject& Shot, float Time)>* PoseAt, const float Time)
	{
		const FBox Bounds = Body->GetImportedBounds().GetBox();
		const FVector Center = Bounds.GetCenter();
		const double Top = FMath::Max(Bounds.Max.Z, 50.0);
		for (const TPair<const TCHAR*, float> View : { TPair<const TCHAR*, float>(TEXT("front"), -90.0f), TPair<const TCHAR*, float>(TEXT("side"), 0.0f) })
		{
			FMantleThumbnailSubject Shot = FMantleCosmeticThumbnail::AvatarSubject(Body, nullptr);
			if (PoseAt)
			{
				// GarmentPoser poses a garment subject (the garment, then its body): the avatar is drawn as the "garment", following
				// the posed body by bone name, the body itself hidden.
				Shot.SkeletalMeshes = { Body, Body };
				TArray<int32> AllSlots;
				for (int32 Slot = 0; Slot < Body->GetMaterials().Num(); ++Slot)
				{
					AllSlots.Add(Slot);
				}
				Shot.HiddenSlots = { TArray<int32>(), AllSlots };
				(*PoseAt)(Shot, Time);
			}
			Shot.Frame = FBox(FVector(Center.X - 70.0, Center.Y - 70.0, 0.0), FVector(Center.X + 70.0, Center.Y + 70.0, Top + 15.0));
			Shot.bFloor = true;
			// The avatar faces +Y: the front camera looks along -Y.
			Shot.View = FRotator(-4.0f, View.Value, 0.0f);
			TArray<FColor> Pixels;
			FString Error;
			const FString File = FPaths::Combine(Directory, FString::Printf(TEXT("%s_%s.png"), *Label, View.Key));
			const bool bWritten = FMantleCosmeticThumbnail::Capture(Shot, Size, Pixels, Error)
				&& FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Size, Size, Pixels), *File);
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK picture %s %s %s"), View.Key, bWritten ? *File : TEXT("FAILED"), bWritten ? TEXT("") : *Error);
		}
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantleAsIsCheck, Error, TEXT("MANTLE_ASIS_CHECK usage: Mantle.Dev.AsIsCheck <SkeletalMesh> <Directory> <Label> [anim=<AnimSequence>] [size]"));
			return;
		}
		FString ClipPath;
		int32 Size = 1024;
		for (int32 Index = 3; Index < Args.Num(); ++Index)
		{
			if (!Args[Index].Split(TEXT("anim="), nullptr, &ClipPath))
			{
				Size = FMath::Clamp(FCString::Atoi(*Args[Index]), 128, 2048);
			}
		}
		FString Error;
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&Error);
		if (!Contract)
		{
			UE_LOG(LogMantleAsIsCheck, Error, TEXT("MANTLE_ASIS_CHECK failed: no contract (%s)"), *Error);
			return;
		}
		FMantleConvertOptions Options;
		Options.Route = EMantleAvatarRoute::AsIs;
		Options.Slot = TEXT("avatar");
		Options.OutputFolder = TEXT("/Game/AsIsCheck/") + Args[2];
		Options.bSave = false;
		Options.bCheck = true;
		FMantleConvertContext Context;
		if (!FMantleAvatarConverter::MakeContextForPath(Args[0], Options, *Contract, Context, Error))
		{
			UE_LOG(LogMantleAsIsCheck, Error, TEXT("MANTLE_ASIS_CHECK failed: %s"), *Error);
			return;
		}
		// The reference: the source mesh as it is, before anything converts it.
		FString NoRender;
		const bool bCanRender = FMantleCosmeticThumbnail::CanCapture(&NoRender);
		if (bCanRender)
		{
			IFileManager::Get().MakeDirectory(*Args[1], /*Tree*/ true);
			for (const FMantleConvertPart& Part : Context.Parts)
			{
				if (Part.Slot == TEXT("avatar") && Part.Mesh.IsValid())
				{
					AsIsShots(Part.Mesh.Get(), Args[1], Args[2] + TEXT("_source"), Size, nullptr, 0.0f);
				}
			}
		}
		const TArray<FMantleConvertFix> Fixes = FMantleAvatarConverter::Analyze(Context);
		const TArray<FString> Keys = FMantleAvatarConverter::FixKeys(Fixes);
		for (int32 Index = 0; Index < Fixes.Num(); ++Index)
		{
			const FMantleConvertFix& Fix = Fixes[Index];
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK fix %s [%s%s]: %s -> %s"), *Keys[Index], Fix.bEnabled ? TEXT("ticked") : TEXT("unticked"),
				Fix.bCanFix ? TEXT("") : TEXT(", can't fix"), *Fix.Problem, *Fix.Action);
		}
		const FMantleConvertResult Result = FMantleAvatarConverter::Convert(Context, Fixes, Options, *Contract);
		AsIsLogOutcomes(TEXT("applied"), Result.Applied);
		AsIsLogOutcomes(TEXT("FAILED"), Result.Failed);
		AsIsLogOutcomes(TEXT("left alone"), Result.LeftAlone);
		for (const FString& Note : Context.Notes)
		{
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK note: %s"), *Note);
		}
		for (const FMantleConvertPartCheck& Check : Result.Checks)
		{
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK part %d: %s, standing height %s"), Check.PartIndex, Check.bCanUpload ? TEXT("PASSES the upload checks") : TEXT("REFUSED"),
				Check.StandingHeightCm.IsSet() ? *FString::Printf(TEXT("%.1f cm"), Check.StandingHeightCm.GetValue()) : TEXT("not measured"));
			for (const FMantleCosmeticIssue& Issue : Check.Issues)
			{
				UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK issue %s: %s"), AsIsSeverity(Issue.Severity), *Issue.ToString());
			}
		}

		USkeletalMesh* Avatar = nullptr;
		for (const FMantleConvertPart& Part : Context.Parts)
		{
			if (Part.Slot == TEXT("avatar") && Part.Mesh.IsValid())
			{
				Avatar = Part.Mesh.Get();
			}
		}
		if (!Avatar)
		{
			UE_LOG(LogMantleAsIsCheck, Error, TEXT("MANTLE_ASIS_CHECK failed: no avatar part after Convert"));
			return;
		}
		const FBox Bounds = Avatar->GetImportedBounds().GetBox();
		const FSkeletalMeshModel* Imported = Avatar->GetImportedModel();
		const int32 Sections = Imported && Imported->LODModels.Num() > 0 ? Imported->LODModels[0].Sections.Num() : -1;
		TArray<FString> Morphs;
		for (const UMorphTarget* Morph : Avatar->GetMorphTargets())
		{
			if (Morph)
			{
				Morphs.Add(Morph->GetName());
			}
		}
		const FVector Extent = Bounds.GetSize();
		UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK result %s: width %.1f cm, depth %.1f cm, height %.1f cm, %d LOD 0 sections, %d morph targets (%s)"),
			*Avatar->GetPathName(), Extent.X, Extent.Y, Extent.Z, Sections, Morphs.Num(), *FString::Join(Morphs, TEXT(", ")));

		if (!bCanRender)
		{
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK no pictures: nothing can render (%s)"), *NoRender);
			UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK done"));
			return;
		}
		AsIsShots(Avatar, Args[1], Args[2], Size, nullptr, 0.0f);
		if (!ClipPath.IsEmpty())
		{
			if (!FPackageName::GetShortName(ClipPath).Contains(TEXT(".")))
			{
				ClipPath += TEXT(".") + FPackageName::GetShortName(ClipPath);
			}
			UAnimSequence* Clip = LoadObject<UAnimSequence>(nullptr, *ClipPath);
			TFunction<void(FMantleThumbnailSubject& Shot, float Time)> PoseAt;
			float Duration = 0.0f;
			if (!Clip || !FMantleEmoteCheck::GarmentPoser(Clip, Avatar, PoseAt, Duration, Error))
			{
				UE_LOG(LogMantleAsIsCheck, Error, TEXT("MANTLE_ASIS_CHECK no %s pictures: %s"), *ClipPath, Clip ? *Error : TEXT("the clip did not load"));
			}
			else
			{
				AsIsShots(Avatar, Args[1], Args[2] + TEXT("_") + Clip->GetName(), Size, &PoseAt, Duration * 0.25f);
			}
		}
		UE_LOG(LogMantleAsIsCheck, Display, TEXT("MANTLE_ASIS_CHECK done"));
	}

	FAutoConsoleCommand GMantleDevAsIsCheckCommand(
		TEXT("Mantle.Dev.AsIsCheck"),
		TEXT("Mantle.Dev.AsIsCheck <SkeletalMesh> <Directory> <Label> [anim=<AnimSequence>] [size] -- converts a mesh As-is as Convert does (nothing saved), logs the fixes, ")
		TEXT("the upload checks, depth, sections and morphs, and draws it front and side as the catalog thumbnail does (and posed in anim=)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
