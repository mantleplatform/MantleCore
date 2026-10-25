// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Compile with the runtime PocketPy parser without executing creator code. Game-thread only. */
MANTLESCRIPTTOOLS_API bool MantlePocketPyCheckSource(const FString& Path, const FString& Source, FString& OutError);
