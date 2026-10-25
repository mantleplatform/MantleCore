// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleNativeEye.h"

class UMaterialInterface;
class UTexture2D;

/** Capture resolved values and four generated textures; validate the four stock inputs against shipped content. */
struct MANTLEEDITOR_API FMantleNativeEyeExport
{
	static bool IsNative(const UMaterialInterface* Material);
	/** Direct stock substitution belongs only to a paired completed Convert output, never As-Is. */
	static bool AllowsSharedProfile(const UObject* Body, const UObject* Face);
	static bool Read(const UMaterialInterface* Material, FMantleNativeEyeSettings& Out,
		TMap<FName, UTexture2D*>& OutTextures, FString& Error);
};
