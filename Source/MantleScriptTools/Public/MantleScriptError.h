// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** In MantleScriptTools so the editor's syntax checks report errors without MantleScript. */
enum class EMantleScriptErrorKind : uint8
{
	Compile,
	Runtime,
	MemoryBudgetExceeded,
	InstructionBudgetExceeded
};

struct FMantleScriptError
{
	EMantleScriptErrorKind Kind = EMantleScriptErrorKind::Runtime;
	FString Message;
};
