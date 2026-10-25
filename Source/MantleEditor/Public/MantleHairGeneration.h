// Copyright Joshua Eger. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"

/** Explicit editor authoring action. Does not enable plugins or install Python packages. */
class MANTLEEDITOR_API FMantleHairGeneration
{
public:
	static bool CanGenerate(const UObject* Groom, FString& Why);
	/** Opens the native generator's settings/progress UI with the slot's triangle cap and a 2K atlas budget. */
	static bool Generate(UObject* Groom, FString& Error, const FString& Slot = TEXT("hair"));
};
