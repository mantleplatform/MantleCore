// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntime.h"

// The module's registration, apart from MantleRuntime.cpp so the game's copy of this module (MantleGameRuntime) can
// register under its own name and share every other file.
IMPLEMENT_MODULE(FMantleRuntimeModule, MantleRuntime)
