// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePocketPySourceCheck.h"

#include "Containers/StringConv.h"

THIRD_PARTY_INCLUDES_START
#include "pocketpy.h"
#include "MantlePocketPyHooks.h"
THIRD_PARTY_INCLUDES_END

namespace
{
	// Finds a free VM slot, or reuses whichever is current if every slot is a live world VM, and
	// leaves PocketPy initialized and switched to it. Slot ownership and the process-wide "first
	// py_initialize()" flag are PocketPyLib's (MantlePocketPyHooks.h), shared with MantleScript's
	// world VMs. bOutScratchSlot is true if the caller owns this slot outright and should
	// py_resetvm() it and give it back when done, false if it must leave a live world VM's state
	// untouched.
	int32 AcquireScratchOrSharedSlot(bool& bOutScratchSlot)
	{
		int32 Slot = -1;
		for (int32 i = 0; i < MANTLE_POCKETPY_MAX_VM; ++i)
		{
			if (!MantlePocketPy_SlotInUse[i]) { Slot = i; break; }
		}
		bOutScratchSlot = Slot >= 0;
		if (!bOutScratchSlot)
		{
			// Every slot is a live world VM; running against "the current VM" still can't
			// corrupt another one, but shares its memory/instruction accounting for the
			// duration of the call, and py_resetvm() is skipped so its own state survives.
			Slot = MantlePocketPy_CurrentVm;
			py_switchvm(Slot);
		}
		else if (MantlePocketPy_EverInitialized)
		{
			// A later VM slot: py_switchvm(Slot) BEFORE py_initialize() so that call initializes
			// THIS slot instead of re-touching whichever one is already current.
			py_switchvm(Slot);
			py_initialize();
		}
		else
		{
			py_initialize(); // the very first py_initialize() call process-wide; always slot 0.
		}
		MantlePocketPy_EverInitialized = 1;
		if (bOutScratchSlot)
		{
			MantlePocketPy_SlotInUse[Slot] = 1;
		}
		MantlePocketPy_CurrentVm = Slot;
		MantlePocketPy_OpCount[Slot] = 0;
		MantlePocketPy_InstructionCount[Slot] = 0;
		return Slot;
	}
}

// =================================================================================================
// MantlePocketPyCheckSource: compile-only ("py_compile-equivalent") check that never runs creator
// code. The editor's script check (MantleScriptCheck.cpp) calls it as a free function.
//
// Uses AcquireScratchOrSharedSlot() (a scratch VM slot when one is free) so a check can run (and
// be torn down) without disturbing a world's own running scripts -- py_compile() only
// syntax-checks and bytecode-compiles Source, per PocketPy's own documented contract; it never
// executes it, so the runtime's instruction-budget hook is not needed here.
bool MantlePocketPyCheckSource(const FString& Path, const FString& Source, FString& OutError)
{
	bool bScratchSlot = false;
	const int32 Slot = AcquireScratchOrSharedSlot(bScratchSlot);
	MantlePocketPy_QuotaCell* const PreviousCell = MantlePocketPy_CurrentQuotaCell;
	const int PreviousInstance = MantlePocketPy_CurrentInstance;
	MantlePocketPy_CurrentQuotaCell = nullptr;
	MantlePocketPy_CurrentInstance = 0;
	const bool bHadAllocationFatal =
		MantlePocketPy_HasAllocationFatal(MantlePocketPy_CurrentVm) != 0;
	const bool bOk = py_compile(TCHAR_TO_UTF8(*Source), TCHAR_TO_UTF8(*Path), EXEC_MODE, false);
	if (!bOk)
	{
		if (py_checkexc())
		{
			char* Msg = py_formatexc();
			py_clearexc(nullptr);
			if (Msg)
			{
				OutError = FString(UTF8_TO_TCHAR(Msg));
				py_free(Msg);
			}
			else
			{
				MantlePocketPy_FatalRecord Fatal{};
				const bool bAllocationFatal = !bHadAllocationFatal &&
					MantlePocketPy_TakeAllocationFatal(MantlePocketPy_CurrentVm, &Fatal) != 0;
				OutError = bAllocationFatal
					? TEXT("PocketPy allocation failed while formatting the validation exception.")
					: TEXT("PocketPy validation exception formatting failed.");
			}
		}
		else
		{
			// SourceData admission failure deliberately has no Python exception.
			// This compile-only entry has no RunBudgeted checkpoint, so drain the
			// fixed POD record before allocating the host-facing diagnostic.
			MantlePocketPy_FatalRecord Fatal{};
			const bool bAllocationFatal =
				!bHadAllocationFatal &&
				MantlePocketPy_TakeAllocationFatal(MantlePocketPy_CurrentVm, &Fatal) != 0;
			OutError = bAllocationFatal
				? TEXT("PocketPy allocation failed while validating source.")
				: TEXT("PocketPy compilation failed without an exception.");
		}
	}
	if (bScratchSlot)
	{
		py_resetvm(); // scratch-only compile: leave no state behind for the next check or world VM.
		MantlePocketPy_SlotInUse[Slot] = 0;
	}
	MantlePocketPy_CurrentQuotaCell = PreviousCell;
	MantlePocketPy_CurrentInstance = PreviousInstance;
	return bOk;
}
