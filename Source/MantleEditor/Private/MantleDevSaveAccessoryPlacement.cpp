// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.SaveAccessoryPlacement <StaticMesh> <Slot> <X> <Y> <Z> [<Pitch> <Yaw> <Roll>] [<Scale>]
// Saves an accessory's placement on its slot exactly as the cosmetics panel saves one (FMantleAccessorySettings::Save, into
// Saved/Config/MantleAccessoryPlacement.ini), so every later draft of that mesh on that slot loads it (FMantleAccessorySettings::Load):
// the panel, Mantle.Dev.RenderAccessory and Mantle.Dev.RenderHairRule. The location is in cm in the socket's frame. Without a
// rotation the draft's default is kept (aligned to the avatar's axes, as the panel starts it); without a scale, 1. Each part is
// clamped to the panel's range as the panel does. Logs MANTLE_SAVE_PLACEMENT with the numbers saved (or "failed").
#include "MantleAccessorySettings.h"
#include "MantleAvatarContract.h"
#include "MantleEditorActions.h"

#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleSavePlacement, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevSaveAccessoryPlacementPrivate
{
	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() != 5 && Args.Num() != 6 && Args.Num() != 8 && Args.Num() != 9)
		{
			UE_LOG(LogMantleSavePlacement, Error, TEXT("MANTLE_SAVE_PLACEMENT failed: usage: Mantle.Dev.SaveAccessoryPlacement <StaticMesh> <Slot> <X> <Y> <Z> [<Pitch> <Yaw> <Roll>] [<Scale>]"));
			return;
		}
		FString Path = Args[0];
		if (!FPackageName::GetShortName(Path).Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
		UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, *Path);
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		const FString& Slot = Args[1];
		if (!Source || !Contract || !Contract->FindSlot(Slot))
		{
			UE_LOG(LogMantleSavePlacement, Error, TEXT("MANTLE_SAVE_PLACEMENT failed: %s is not a static mesh, there is no contract, or %s is not a slot"), *Path, *Slot);
			return;
		}
		// The draft the panel would start with (a saved placement, else aligned to the avatar's axes), then the numbers given.
		FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Accessory, Source, Slot, FString(), *Contract, EMantleFitSex::Auto);
		const auto Number = [&Args](int32 Index) { return static_cast<float>(FCString::Atod(*Args[Index])); };
		Draft.AttachLocation = FMantleCosmeticPackage::ClampAttachLocation(FVector3f(Number(2), Number(3), Number(4)));
		if (Args.Num() >= 8) Draft.AttachRotation = FMantleCosmeticPackage::ClampAttachRotation(FVector3f(Number(5), Number(6), Number(7)));
		const int32 ScaleArg = Args.Num() == 6 ? 5 : (Args.Num() == 9 ? 8 : INDEX_NONE);
		Draft.AttachScale = FMantleCosmeticPackage::ClampAttachScale(ScaleArg == INDEX_NONE ? FVector3f::OneVector : FVector3f(Number(ScaleArg)));
		Draft.bAttach = true;
		const FString File = FMantleAccessorySettings::DefaultConfigFile();
		FMantleAccessorySettings::Save(Draft, File);
		FMantleCosmeticDraft Check = Draft;
		if (!FMantleAccessorySettings::Load(Check, File) || !Check.AttachLocation.Equals(Draft.AttachLocation, 1e-3f) || !Check.AttachRotation.Equals(Draft.AttachRotation, 1e-3f))
		{
			UE_LOG(LogMantleSavePlacement, Error, TEXT("MANTLE_SAVE_PLACEMENT failed: the placement did not read back from %s"), *File);
			return;
		}
		UE_LOG(LogMantleSavePlacement, Display, TEXT("MANTLE_SAVE_PLACEMENT %s on %s: location=%s rotation=%s scale=%s into %s"), *Source->GetPathName(), *Slot,
			*Draft.AttachLocation.ToString(), *Draft.AttachRotation.ToString(), *Draft.AttachScale.ToString(), *File);
	}

	FAutoConsoleCommand GMantleDevSaveAccessoryPlacementCommand(
		TEXT("Mantle.Dev.SaveAccessoryPlacement"),
		TEXT("Mantle.Dev.SaveAccessoryPlacement <StaticMesh> <Slot> <X> <Y> <Z> [<Pitch> <Yaw> <Roll>] [<Scale>] -- saves an accessory's placement on its slot ")
		TEXT("as the cosmetics panel does, so the panel and the render commands load it. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}
