// Copyright Joshua Eger. All Rights Reserved.
/* Not part of upstream PocketPy. Host-side (Mantle) support for the PK_MALLOC/PK_REALLOC/
 * PK_FREE macro seam PocketPyLib.Build.cs wires up, plus the per-VM accounting
 * MantlePocketPy.cpp (MantleScript) reads. Ported from ScriptLab's PocketPyHooks.h (T-Python
 * evaluation); Lab* renamed to Mantle* throughout, per CLAUDE.md's porting rule. See
 * PocketPyLib/VERSION for the full explanation.
 *
 * Memory is soft-capped: PocketPy never null-checks its allocator's return (confirmed by the
 * lab's EVALUATION.md bug-hunt notes), so a hard-denying allocator risks exactly the heap
 * corruption that evaluation found -- and setjmp/longjmp across UE C++ frames is forbidden (it
 * would skip destructors), which is how the lab's own hard cap recovered. Instead: every
 * allocation is honoured (so PocketPy's internal state stays consistent), but once a VM's
 * accounted usage crosses its limit, MantlePocketPy_MemOverBudget[vm] is set. MantlePocketPy.cpp
 * polls that flag at the one safe checkpoint it controls -- right after each top-level
 * py_exec()/py_next() resume returns, never from inside the allocator itself -- and raises a
 * Python MemoryError there (or stops the offending script if the VM looks unrecoverable), then
 * clears the flag.
 */
#pragma once

#include <stddef.h>

/* Modular (non-monolithic) UE targets put every module behind a shared-library boundary, so a
 * plain `extern` isn't visible across the PocketPyLib.dll / MantleScript.dll edge -- same
 * situation LuauLib's LUA_API/LUACODE_API solves (see LuauLib.Build.cs). POCKETPY_HOOKS_API is
 * dllexport when this header is compiled as part of PocketPyLib itself (MantlePocketPyAlloc.c,
 * via PocketPyLib.Build.cs's PrivateDefinitions) and dllimport for any other module that
 * consumes it (MantleScript, via its own Build.cs); monolithic Game/Server targets need neither. */
#ifndef POCKETPY_HOOKS_API
#define POCKETPY_HOOKS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MANTLE_POCKETPY_MAX_VM 16
#define MANTLE_POCKETPY_GC_DEBT_RING 32

/* #158 production boundary capture is independent of optional detailed GC diagnostics.
 * It is called immediately around every real collection and must only write fixed POD. */
typedef void (*MantlePocketPy_GcBoundaryFn)(int vm, int automatic, int begin, int freed);
extern POCKETPY_HOOKS_API MantlePocketPy_GcBoundaryFn MantlePocketPy_GcBoundary[MANTLE_POCKETPY_MAX_VM];

/* Pure size query for checked container layout arithmetic. Includes the actual
 * private allocator header; does not allocate or change accounting. */
POCKETPY_HOOKS_API size_t MantlePocketPy_AllocationHeaderBytes(void);

/* Which VM slot allocations are currently charged to. MantlePocketPy.cpp sets this immediately
 * before (and leaves it set after) every py_switchvm() call, so it always matches PocketPy's
 * own "current VM" -- there is no public py_getvmindex(), so the host has to mirror it. */
extern POCKETPY_HOOKS_API int MantlePocketPy_CurrentVm;

/* VM slot ownership, shared by every module that drives PocketPy in this process (the
 * runtime's world VMs in MantleScript, and the compile-only source check in MantleScriptTools).
 * py_initialize() sets up the implicit default VM (slot 0) the first time it is ever called
 * process-wide; every later slot must py_switchvm(NewSlot) BEFORE calling py_initialize() again,
 * so that call initializes THAT slot. MantlePocketPy_EverInitialized records the first call;
 * MantlePocketPy_SlotInUse[slot] is nonzero while a world VM or a check owns the slot. */
extern POCKETPY_HOOKS_API int MantlePocketPy_EverInitialized;
extern POCKETPY_HOOKS_API int MantlePocketPy_SlotInUse[MANTLE_POCKETPY_MAX_VM];

#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
/* Game-thread-only, disabled by default. Tests save/restore these POD counters;
 * no observer callback, allocator replacement, denial, or ownership change.
 * Entries[0/1/2] count malloc/realloc/free calls, including null/no-op calls. */
extern POCKETPY_HOOKS_API int MantlePocketPy_RootAuditEnabled;
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_RootAllocatorEntries[3];
#endif

#if MANTLE_POCKETPY_GC_DIAGNOSTICS
/* Optional collector-boundary observer. automatic=1 is a threshold-triggered collection;
 * automatic=0 is explicit py_gc_collect (including script gc.collect). begin=0 supplies
 * the completed sweep's freed count. Observers must not allocate, call Python, or log. */
typedef void (*MantlePocketPy_GcObserverFn)(int vm, int automatic, int begin, int freed);
extern POCKETPY_HOOKS_API MantlePocketPy_GcObserverFn MantlePocketPy_GcObserver[MANTLE_POCKETPY_MAX_VM];

/* #158: optional subphase observer, nested inside the whole-GC observer above.
 * Roots includes native root traversal; Graph walks the resulting object stack.
 * Sweep phases include native destructors/finalizers (not a separate additive cost).
 * End supplies work = queued root objects / traversed graph objects / 0 for small
 * sweep / scanned large objects, and freed = reclaimed objects for sweep phases.
 * No clock reads per object; the observer may only read a clock and update POD.
 * These phases do not change collection policy or any CPU charge. */
typedef enum MantlePocketPy_GcPhase {
    MantlePocketPy_GcRoots = 0,
    MantlePocketPy_GcGraph,
    MantlePocketPy_GcSmallSweep,
    MantlePocketPy_GcLargeSweep,
    MantlePocketPy_GcPhaseCount
} MantlePocketPy_GcPhase;
typedef void (*MantlePocketPy_GcPhaseObserverFn)(int vm, int phase, int begin,
                                               unsigned long long work, unsigned long long freed);
extern POCKETPY_HOOKS_API MantlePocketPy_GcPhaseObserverFn MantlePocketPy_GcPhaseObserver[MANTLE_POCKETPY_MAX_VM];

/* Small-sweep detail is nested inside SmallSweep. Sample ordinal 1 then every 256th
 * dead object in each category; sampled seconds are an observed subset, NEVER a total
 * estimate. All state is fixed POD; callback/clock must not allocate or run scripts. */
#define MANTLE_POCKETPY_DTOR_SAMPLE_CADENCE 256
enum { MantlePocketPy_SweepList, MantlePocketPy_SweepDict, MantlePocketPy_SweepString,
       MantlePocketPy_SweepTuple, MantlePocketPy_SweepHostHandle, MantlePocketPy_SweepOther,
       MantlePocketPy_SweepCategoryCount };
typedef struct MantlePocketPy_GcSweepStats {
    unsigned long long pools, arenas, blocks, free_blocks, live_blocks, dead_blocks;
    unsigned long long empty_arenas, empty_blocks, released_arenas, allocator_free_calls, accounting_calls;
    unsigned long long dead[MantlePocketPy_SweepCategoryCount];
    unsigned long long samples[MantlePocketPy_SweepCategoryCount];
    double sample_seconds[MantlePocketPy_SweepCategoryCount];
    unsigned long long allocator_samples[MantlePocketPy_SweepCategoryCount];
    double charge_sample_seconds[MantlePocketPy_SweepCategoryCount];
    double free_sample_seconds[MantlePocketPy_SweepCategoryCount];
    double charge_sample_max_seconds[MantlePocketPy_SweepCategoryCount];
    double free_sample_max_seconds[MantlePocketPy_SweepCategoryCount];
    double scan_seconds, release_seconds;
    /* Internal sampling scope only; -1 except while a selected destructor runs.
     * Allocator samples partition actual selected frees into charge then libc free. */
    int active_sample_category;
} MantlePocketPy_GcSweepStats;
typedef double (*MantlePocketPy_GcClockFn)(void);
typedef void (*MantlePocketPy_GcSweepObserverFn)(int vm, const MantlePocketPy_GcSweepStats* stats);
extern POCKETPY_HOOKS_API MantlePocketPy_GcClockFn MantlePocketPy_GcClock[MANTLE_POCKETPY_MAX_VM];
extern POCKETPY_HOOKS_API MantlePocketPy_GcSweepObserverFn MantlePocketPy_GcSweepObserver[MANTLE_POCKETPY_MAX_VM];
extern POCKETPY_HOOKS_API void (*MantlePocketPy_GcHandleDtor[MANTLE_POCKETPY_MAX_VM])(void*);
/* Non-null only during observed small sweep; allocator counts the CURRENT collection,
 * while normal accounting continues to charge the original allocation header owner. */
extern POCKETPY_HOOKS_API MantlePocketPy_GcSweepStats* MantlePocketPy_GcActiveSweep[MANTLE_POCKETPY_MAX_VM];
#endif

extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_MemUsed[MANTLE_POCKETPY_MAX_VM];
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_MemReserved[MANTLE_POCKETPY_MAX_VM];
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_MemLimit[MANTLE_POCKETPY_MAX_VM]; /* 0 = unlimited */
extern POCKETPY_HOOKS_API int MantlePocketPy_MemOverBudget[MANTLE_POCKETPY_MAX_VM];

/* #153: stable per-generation quota state. The host creates a cell at a healthy
 * boundary before the instance's first PocketPy allocation, and retains it until
 * every header from that generation has been released. Allocator admission reads
 * and writes only this POD plus the fixed per-VM arrays above: no map lookup,
 * allocation, callback, logging or Python API is permitted. */
typedef struct MantlePocketPy_GcDebt {
	unsigned long long epoch;
	unsigned long long bytes;
	unsigned long long calls;
} MantlePocketPy_GcDebt;

typedef struct MantlePocketPy_QuotaCell {
    unsigned long long committed;
    unsigned long long reserved;
    unsigned long long limit; /* 0 = unlimited */
    unsigned long long generation;
	/* Positive allocation/reallocation volume per bounded #158 epoch. Each slot
	 * remains tied to this exact generation after numeric ID retirement. */
	MantlePocketPy_GcDebt gc_debt[MANTLE_POCKETPY_GC_DEBT_RING];
    int vm;
    int instance;
    int active;
} MantlePocketPy_QuotaCell;

/* Advanced by every POD full-GC boundary capture because either automatic or explicit
 * collection resets PocketPy's counter. Automatic events settle their closed epoch.
 * 0 means no active VM epoch. */
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_GcDebtEpoch[MANTLE_POCKETPY_MAX_VM];
extern POCKETPY_HOOKS_API MantlePocketPy_GcDebt MantlePocketPy_GcUnownedDebt[MANTLE_POCKETPY_MAX_VM][MANTLE_POCKETPY_GC_DEBT_RING];
extern POCKETPY_HOOKS_API int MantlePocketPy_GcDebtOverflow[MANTLE_POCKETPY_MAX_VM];

/* Mirrors MantlePocketPy_CurrentInstance but identifies its exact generation.
 * NULL is used for VM/bootstrap allocations and by legacy native accounting tests. */
extern POCKETPY_HOOKS_API MantlePocketPy_QuotaCell* MantlePocketPy_CurrentQuotaCell;

typedef enum MantlePocketPy_AdmissionResult {
    MantlePocketPy_AdmissionOk = 0,
    MantlePocketPy_AdmissionInvalid,
    MantlePocketPy_AdmissionSizeOverflow,
    MantlePocketPy_AdmissionVmLimit,
    MantlePocketPy_AdmissionInstanceLimit,
    MantlePocketPy_AdmissionAllocFailed
} MantlePocketPy_AdmissionResult;

/* Allocation-independent terminal delivery for converted fallible paths. The
 * first record in a VM slot wins until the host consumes or resets it. Identity
 * is copied, not referenced, so retiring/pruning a quota cell cannot retarget a
 * pending fault at a replacement generation. Game-thread-only, fixed POD. */
typedef struct MantlePocketPy_FatalRecord {
    unsigned long long generation;
    int vm;
    int instance;
    int reason; /* MantlePocketPy_AdmissionResult */
    int pending;
} MantlePocketPy_FatalRecord;

typedef struct MantlePocketPy_Reservation {
    MantlePocketPy_QuotaCell* cell;
    unsigned long long bytes;
    int vm;
    int active;
} MantlePocketPy_Reservation;

typedef void* (*MantlePocketPy_MallocFn)(size_t size, void* context);
typedef void* (*MantlePocketPy_ReallocFn)(void* ptr, size_t size, void* context);
typedef void (*MantlePocketPy_FreeFn)(void* ptr, void* context);

POCKETPY_HOOKS_API void MantlePocketPy_InitQuotaCell(
    MantlePocketPy_QuotaCell* cell, int vm, int instance,
    unsigned long long generation, unsigned long long limit);
POCKETPY_HOOKS_API void MantlePocketPy_RetireQuotaCell(MantlePocketPy_QuotaCell* cell);
POCKETPY_HOOKS_API void MantlePocketPy_RecordAllocationFatal(
    MantlePocketPy_AdmissionResult reason, int vm, int instance,
    unsigned long long generation);
/* Take only copies and clears POD. Production hosts may call it solely at a
 * checkpoint after the PocketPy/native/C++ work beneath the resume returned. */
POCKETPY_HOOKS_API int MantlePocketPy_TakeAllocationFatal(
    int vm, MantlePocketPy_FatalRecord* out);
/* Read-only global observation for host diagnostics/tests. It is not terminal
 * authority because the pending record may belong to a sibling generation.
 * Fixed-array/POD access; it neither consumes nor allocates. */
POCKETPY_HOOKS_API int MantlePocketPy_HasAllocationFatal(int vm);
/* Terminal propagation is generation-local: a pending record owned by a
 * sibling, retired instance or same-ID predecessor must stay queued for the
 * host checkpoint without poisoning the currently executing quota cell. */
POCKETPY_HOOKS_API int MantlePocketPy_HasAllocationFatalForCell(
    int vm, const MantlePocketPy_QuotaCell* cell);
POCKETPY_HOOKS_API void MantlePocketPy_ResetAllocationFatal(int vm);

/* Reserve is the allocation-time ceiling decision. Success transfers bytes from
 * available quota to reserved quota; Commit moves the same bytes to committed,
 * and Rollback releases them. Outputs remain unchanged on failure. These calls
 * are game-thread-only and allocation-independent. */
POCKETPY_HOOKS_API MantlePocketPy_AdmissionResult MantlePocketPy_TryReserve(
    int vm, MantlePocketPy_QuotaCell* cell, unsigned long long bytes,
    MantlePocketPy_Reservation* out);
POCKETPY_HOOKS_API void MantlePocketPy_CommitReservation(MantlePocketPy_Reservation* reservation);
POCKETPY_HOOKS_API void MantlePocketPy_RollbackReservation(MantlePocketPy_Reservation* reservation);

/* Fallible allocator path for converted callers. The allocator callbacks receive
 * the complete private-header size. A denied request never invokes them. Failed
 * malloc/realloc rolls the reservation back; realloc failure preserves the old
 * user pointer/header/charge. Runtime allocation failures also publish the
 * first exact-owner fatal record for later safe host consumption. Admitted
 * realloc rejects a legacy numeric-owner block because changing its debit route
 * would corrupt legacy accounting.
 * FreeWith debits the header's original generation.
 * Production PK_MALLOC/PK_REALLOC remain on the non-denying wrappers below until
 * the upstream caller graph has been converted to propagate these results. */
POCKETPY_HOOKS_API MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedMalloc(
    size_t size, int vm, MantlePocketPy_QuotaCell* cell,
    MantlePocketPy_MallocFn allocate, void* context, void** out);
/* Admit a distinct allocation against an existing admitted block's immutable
 * owner identity. This is for failure-atomic rebuilds which cannot realloc in
 * place (for example, an open-addressed hash table). Numeric legacy-owner
 * blocks are rejected; outputs remain unchanged on failure. */
POCKETPY_HOOKS_API MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedMallocLike(
    const void* owner, size_t size, MantlePocketPy_MallocFn allocate,
    void* context, void** out);
POCKETPY_HOOKS_API MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedRealloc(
    void* ptr, size_t size, MantlePocketPy_ReallocFn reallocate, void* context, void** out);
POCKETPY_HOOKS_API void MantlePocketPy_FreeWith(
    void* ptr, MantlePocketPy_FreeFn release, void* context);
POCKETPY_HOOKS_API int MantlePocketPy_GetAllocationOwner(
    const void* ptr, int* vm, int* instance, unsigned long long* generation,
    unsigned long long* total_bytes);

/* Transfer an admitted allocation from its exact temporary quota cell to its
 * original VM's process-lifetime ownership.  The VM charge remains committed;
 * only the cell charge and retained cell pointer are cleared.  This is for
 * process-global tables whose entries outlive every instance cell.  It does
 * not allocate, free, invoke host callbacks, or publish a fatal. */
POCKETPY_HOOKS_API int MantlePocketPy_DetachAllocationCell(
    void* ptr, MantlePocketPy_QuotaCell* expected_cell);

POCKETPY_HOOKS_API void* MantlePocketPy_Malloc(size_t size);
POCKETPY_HOOKS_API void* MantlePocketPy_Realloc(void* ptr, size_t size);
POCKETPY_HOOKS_API void MantlePocketPy_Free(void* ptr);

/* ---- MANTLE PATCH (see VERSION and patches/README.txt): deterministic per-opcode instruction
 * budget. `py_sys_settrace`'s TRACE_EVENT_LINE only fires when the *source line* changes
 * (pocketpy.c's VM__run_top_frame) -- a `while True: x = x + 1`-shaped loop can revisit the same
 * bytecode indefinitely without ever crossing a line boundary the tracer notices, so a budget
 * built on settrace alone never fires and a runaway script hangs the whole game thread. This
 * counts every opcode instead, from inside pocketpy.c's own dispatch loop:
 *
 *   MantlePocketPy_OpCount[vm] increments once per opcode (a single integer increment, no
 *   function call) and, only once every MANTLE_POCKETPY_INTERRUPT_PERIOD opcodes,
 *   MantlePocketPy_Interrupt -- a host-installed function pointer -- is polled. Returning 0
 *   means "over budget": pocketpy.c raises a RuntimeError and unwinds via `goto __ERROR`
 *   immediately, in the SAME opcode, exactly where the existing PK_ENABLE_WATCHDOG block already
 *   does the same thing for a wall-clock timeout (which MantlePocketPy.cpp keeps enabled too, as
 *   a backstop for anything -- a long native call, say -- this per-opcode counter can't see).
 *
 * MantlePocketPy_Interrupt is a single, not-per-VM function pointer, and the actual budget
 * accumulator (MantlePocketPy_InstructionCount, below) lives here rather than inside
 * MantlePocketPy.cpp's own FMantlePocketPyVm: the hook has to work correctly even for a scratch
 * VM slot with no FMantlePocketPyVm behind it at all (MantlePocketPyCheckSource) -- if the accumulator lived only on a C++ object reached via
 * GVmBySlot[vm], a scratch slot's runaway loop would find no object there and the hook would
 * have nothing to increment, never fault, and hang exactly like the bug this patch exists to
 * fix. MantlePocketPy.cpp's RunBudgeted() still does its own save/reset/restore of this array
 * around each top-level resume, the same way it always did, so a nested resume's usage still
 * never bleeds into its caller's remaining budget. */
#define MANTLE_POCKETPY_INTERRUPT_PERIOD 1024

typedef int (*MantlePocketPy_InterruptFn)(void); /* returns 0 to fault (over budget), nonzero to continue */
extern POCKETPY_HOOKS_API MantlePocketPy_InterruptFn MantlePocketPy_Interrupt;
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_OpCount[MANTLE_POCKETPY_MAX_VM]; /* opcodes since the last hook poll; wraps at the period */
extern POCKETPY_HOOKS_API unsigned long long MantlePocketPy_InstructionCount[MANTLE_POCKETPY_MAX_VM]; /* the actual budget accumulator */
/* ---- END MANTLE PATCH ---- */

/* ---- S1.16 (#132): per-INSTANCE memory accounting, on top of the per-VM accounting above.
 * MantlePocketPy_MemUsed[vm] is one VM's total (every script instance loaded in one world, since
 * MantleScript.cpp -- like MantleLuau.cpp/MantleQuickJS.cpp -- runs one interpreter per (world,
 * language), not one per script). That's the existing per-VM ceiling; this is the new, tighter,
 * per-script one the ticket asks for ("memory per script, not per VM").
 *
 * MantlePocketPy_CurrentInstance mirrors MantlePocketPy_CurrentVm's own pattern: MantlePocketPy.cpp
 * keeps it in sync with FMantlePocketPyVm::InstanceStack (FScopedInstance, already used to attribute
 * a callback to the script that created it) every time that stack changes. Every allocation header
 * below (FMantleAllocHeader) already carries the owning vm; S1.16 adds an instance field the exact
 * same way, so free()/realloc() still charge whichever instance actually OWNS the block, never
 * whichever instance happens to be running when the GC frees it later.
 *
 * Real script allocations now retain CurrentQuotaCell in their private header and update that
 * exact generation directly. No map lookup or callback occurs on that path. The numeric callback
 * remains only for bootstrap and native ownership tests which intentionally have no cell; their
 * compatibility accounting is not used by allocation admission. */
typedef void (*MantlePocketPy_InstanceChargeFn)(int vm, int instance, long long delta);
extern POCKETPY_HOOKS_API MantlePocketPy_InstanceChargeFn MantlePocketPy_InstanceCharge;
extern POCKETPY_HOOKS_API int MantlePocketPy_CurrentInstance;
/* ---- END S1.16 ---- */

#ifdef __cplusplus
}
#endif
