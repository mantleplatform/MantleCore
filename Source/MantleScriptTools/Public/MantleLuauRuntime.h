// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleScriptError.h"

/** Editor-side Luau source validation. The live Luau VM is in Private/Luau/MantleLuau.cpp. */
class MANTLESCRIPTTOOLS_API FMantleLuauRuntime
{
public:
	/** Syntax-checks and bytecode-loads source without executing creator code. */
	static bool ValidateSource(
		const FString& Source,
		FMantleScriptError& OutError,
		uint64 MaximumSourceBytes = 1024 * 1024);
};
