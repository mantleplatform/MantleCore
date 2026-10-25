// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMantleWeaponSource;
struct FMantleWeaponDesc;

/** Turns a saved weapon source into the weapon's published files (#729). The game model goes to /Game/MantleWeapons/<Name> and the weapon's settings file next to it; exporting again replaces them. */
struct MANTLEEDITOR_API FMantleWeaponBuilderExport
{
	/** What happened, in plain words, for the creator. True when the files were written. */
	static bool Export(const UMantleWeaponSource& Source, TArray<FText>& OutMessages);
	/** The same validated v2 document Export writes. Points are already in the fitted weapon frame. No assets are changed. */
	static bool BuildDocument(const UMantleWeaponSource& Source, const TMap<FName, FTransform>& PointsInWeapon,
		const FString& MeshName, FMantleWeaponDesc& Out, TArray<FText>& OutMessages);

	/** Where the eye is when aiming, along the bore from the back of the gun, in cm (Mantle.Weapon.AutoFitAccuracy.EyeFromRear:
	 *  the pack's eye points, TR15 16.5 and held out AK105 14.8, MKR9 13.9, SRM-12 18.0; the WK-11 21.6 behind its rear). */
	static constexpr double LongGunEyeFromRearCm = 16.5;
	static constexpr double PistolEyeFromRearCm = -21.6;
};
