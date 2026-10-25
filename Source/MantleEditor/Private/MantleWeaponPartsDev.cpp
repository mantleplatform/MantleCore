// Copyright Joshua Eger. All Rights Reserved.
// Dev command: `Mantle.Dev.BuildWeaponWithParts <Name> <Hold> <ModelPath> [part=<MeshPath>@<x>:<y>:<z>]... [fire=<SoundWavePath>]
// [quiet=<SoundWavePath>]` makes a builder weapon the way the panel's Create does (the hold's default tuning, auto fit), with
// extra parts (Parts and sound > attachments; each a static or skeletal mesh at a place in the weapon, cm) and its own attack
// sounds, exported to /Game/MantleWeapons/<Name>. A fixture for publishing such a weapon. Logs MANTLE_BUILD_WEAPON_OK or
// MANTLE_BUILD_WEAPON_FAILED with the builder's own messages.

#include "MantleWeaponBuilderExport.h"
#include "MantleWeaponBuilderLogic.h"
#include "MantleWeaponSource.h"
#include "MantleRuntime.h"
#include "HAL/IConsoleManager.h"

namespace MantleWeaponPartsDev
{
	static void BuildWeaponWithParts(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED usage: Mantle.Dev.BuildWeaponWithParts <Name> <Hold> <ModelPath> [part=<MeshPath>@x:y:z]... [fire=<SoundWavePath>] [quiet=<SoundWavePath>]"));
			return;
		}
		const int64 HoldValue = StaticEnum<EMantleWeaponHold>()->GetValueByNameString(Args[1]);
		UObject* Model = LoadObject<UObject>(nullptr, *Args[2]);
		if (HoldValue == INDEX_NONE || !Model)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED unknown hold %s or no model at %s"), *Args[1], *Args[2]);
			return;
		}
		const EMantleWeaponHold Hold = static_cast<EMantleWeaponHold>(HoldValue);
		UMantleWeaponSource* Source = NewObject<UMantleWeaponSource>();
		Source->DisplayName = Args[0];
		Source->Hold = Hold;
		Source->Tuning = MantleWeaponBuilderLogic::DefaultTuning(Hold);
		Source->Model = Model;
		for (int32 Index = 3; Index < Args.Num(); ++Index)
		{
			FString Key, Value;
			if (!Args[Index].Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key == TEXT("part"))
			{
				FString Path, Where;
				if (!Value.Split(TEXT("@"), &Path, &Where))
				{
					Path = Value;
				}
				// x:y:z too, since -ExecCmds splits on commas.
				TArray<FString> Numbers;
				Where.Replace(TEXT(":"), TEXT(",")).ParseIntoArray(Numbers, TEXT(","));
				FMantleWeaponSourceAttachment& Part = Source->Attachments.AddDefaulted_GetRef();
				Part.Name = FString::Printf(TEXT("Part%d"), Source->Attachments.Num());
				Part.Model = FSoftObjectPath(Path);
				Part.RelativeTransform.SetLocation(Numbers.Num() == 3
					? FVector(FCString::Atod(*Numbers[0]), FCString::Atod(*Numbers[1]), FCString::Atod(*Numbers[2])) : FVector::ZeroVector);
			}
			else if (Key == TEXT("fire"))
			{
				Source->FireSound = FSoftObjectPath(Value);
			}
			else if (Key == TEXT("quiet"))
			{
				Source->FireSuppressedSound = FSoftObjectPath(Value);
			}
		}
		TArray<FText> Messages;
		const bool bExported = FMantleWeaponBuilderExport::Export(*Source, Messages);
		FString Words;
		for (const FText& Message : Messages)
		{
			Words += TEXT(" | ") + Message.ToString();
		}
		if (bExported)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_BUILD_WEAPON_OK %s %s%s"), *Args[0], *Args[1], *Words);
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_BUILD_WEAPON_FAILED %s %s%s"), *Args[0], *Args[1], *Words);
		}
	}

	static FAutoConsoleCommand GBuildWeaponWithPartsCommand(
		TEXT("Mantle.Dev.BuildWeaponWithParts"),
		TEXT("Mantle.Dev.BuildWeaponWithParts <Name> <Hold> <ModelPath> [part=<MeshPath>@x:y:z]... [fire=<SoundWavePath>] [quiet=<SoundWavePath>] -- the weapon builder's Create ")
		TEXT("without the panel, with extra parts and its own attack sounds, exported to /Game/MantleWeapons/<Name>. Logs MANTLE_BUILD_WEAPON_OK or MANTLE_BUILD_WEAPON_FAILED."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&BuildWeaponWithParts));
}
