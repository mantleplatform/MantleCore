// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Where a script (or an API member) is allowed to run. */
enum class EMantleSide : uint8
{
	None = 0,
	Server = 1,
	Client = 2,
	Both = 3
};
ENUM_CLASS_FLAGS(EMantleSide);

MANTLESCRIPTTOOLS_API const TCHAR* MantleSideName(EMantleSide Side);
