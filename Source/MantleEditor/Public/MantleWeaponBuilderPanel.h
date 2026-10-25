// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** The "Build a weapon" window's tab (#729): registered at module startup, opened from Tools > Mantle. */
struct MANTLEEDITOR_API FMantleWeaponBuilderPanel
{
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void TryInvokeTab();
};
