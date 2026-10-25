// Copyright Joshua Eger. All Rights Reserved.
/* Not part of upstream PocketPy. Implements the PK_MALLOC/PK_REALLOC/PK_FREE
 * seam and #153's allocation-independent quota reservation foundation.
 *
 * Production allocation remains non-denying until every reachable upstream
 * caller propagates allocation failure. Converted callers can use the admitted
 * API below: quota is reserved before the supplied libc-equivalent callback,
 * committed only after success, and rolled back on failure. */
#include "MantlePocketPyCheckedAlloc.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int MantlePocketPy_CurrentVm = 0;
int MantlePocketPy_EverInitialized = 0;
int MantlePocketPy_SlotInUse[MANTLE_POCKETPY_MAX_VM];
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
int MantlePocketPy_RootAuditEnabled = 0;
unsigned long long MantlePocketPy_RootAllocatorEntries[3];
#endif
unsigned long long MantlePocketPy_MemUsed[MANTLE_POCKETPY_MAX_VM];
unsigned long long MantlePocketPy_MemReserved[MANTLE_POCKETPY_MAX_VM];
unsigned long long MantlePocketPy_MemLimit[MANTLE_POCKETPY_MAX_VM];
int MantlePocketPy_MemOverBudget[MANTLE_POCKETPY_MAX_VM];

#if MANTLE_POCKETPY_GC_DIAGNOSTICS
MantlePocketPy_GcObserverFn MantlePocketPy_GcObserver[MANTLE_POCKETPY_MAX_VM];
MantlePocketPy_GcPhaseObserverFn MantlePocketPy_GcPhaseObserver[MANTLE_POCKETPY_MAX_VM];
MantlePocketPy_GcClockFn MantlePocketPy_GcClock[MANTLE_POCKETPY_MAX_VM];
MantlePocketPy_GcSweepObserverFn MantlePocketPy_GcSweepObserver[MANTLE_POCKETPY_MAX_VM];
void (*MantlePocketPy_GcHandleDtor[MANTLE_POCKETPY_MAX_VM])(void*);
MantlePocketPy_GcSweepStats* MantlePocketPy_GcActiveSweep[MANTLE_POCKETPY_MAX_VM];
#endif

MantlePocketPy_InterruptFn MantlePocketPy_Interrupt = 0;
unsigned long long MantlePocketPy_OpCount[MANTLE_POCKETPY_MAX_VM];
unsigned long long MantlePocketPy_InstructionCount[MANTLE_POCKETPY_MAX_VM];

/* Retained for bootstrap and native ownership tests which intentionally have no
 * generation cell. Real script instances use their stable cell directly. */
MantlePocketPy_InstanceChargeFn MantlePocketPy_InstanceCharge = 0;
int MantlePocketPy_CurrentInstance = 0;
MantlePocketPy_QuotaCell* MantlePocketPy_CurrentQuotaCell = NULL;
static MantlePocketPy_FatalRecord MantlePocketPy_Fatal[MANTLE_POCKETPY_MAX_VM];
MantlePocketPy_GcBoundaryFn MantlePocketPy_GcBoundary[MANTLE_POCKETPY_MAX_VM];
unsigned long long MantlePocketPy_GcDebtEpoch[MANTLE_POCKETPY_MAX_VM];
MantlePocketPy_GcDebt MantlePocketPy_GcUnownedDebt[MANTLE_POCKETPY_MAX_VM][MANTLE_POCKETPY_GC_DEBT_RING];
int MantlePocketPy_GcDebtOverflow[MANTLE_POCKETPY_MAX_VM];

enum { MANTLE_ALLOCATION_NO_LEGACY_CHARGE = 1 };

typedef struct FMantleAllocHeader
{
    unsigned long long size;
    MantlePocketPy_QuotaCell* cell;
    int vm;
    int instance;
    int flags;
} FMantleAllocHeader;

size_t MantlePocketPy_AllocationHeaderBytes(void) { return sizeof(FMantleAllocHeader); }

static int MantlePocketPy_ValidVm(int vm)
{
    return vm >= 0 && vm < MANTLE_POCKETPY_MAX_VM;
}

static int MantlePocketPy_AddOverflow(
    unsigned long long a, unsigned long long b, unsigned long long* out)
{
    if(ULLONG_MAX - a < b) return 1;
    *out = a + b;
    return 0;
}

static MantlePocketPy_AdmissionResult MantlePocketPy_TotalSize(
    size_t payload, size_t* total)
{
    if(!total) return MantlePocketPy_AdmissionInvalid;
    if(payload > SIZE_MAX - sizeof(FMantleAllocHeader))
        return MantlePocketPy_AdmissionSizeOverflow;
    *total = payload + sizeof(FMantleAllocHeader);
    return MantlePocketPy_AdmissionOk;
}

static void MantlePocketPy_ObserveAccounting(void);

static void MantlePocketPy_RecordGcDebtForCell(int vm, MantlePocketPy_QuotaCell* cell,
													 unsigned long long bytes)
{
	unsigned long long epoch;
	MantlePocketPy_GcDebt* debt;
	if(!bytes || !MantlePocketPy_ValidVm(vm)) return;
	epoch = MantlePocketPy_GcDebtEpoch[vm];
	if(!epoch) return;
	if(!cell || !cell->generation || cell->vm != vm)
	{
		debt = &MantlePocketPy_GcUnownedDebt[vm][epoch % MANTLE_POCKETPY_GC_DEBT_RING];
	}
	else
	{
		debt = &cell->gc_debt[epoch % MANTLE_POCKETPY_GC_DEBT_RING];
	}
	if(debt->epoch != epoch)
	{
		if(debt->epoch != 0) MantlePocketPy_GcDebtOverflow[vm] = 1;
		debt->epoch = epoch;
		debt->bytes = 0;
		debt->calls = 0;
	}
	debt->bytes = ULLONG_MAX - debt->bytes < bytes ? ULLONG_MAX : debt->bytes + bytes;
	if(debt->calls != ULLONG_MAX) ++debt->calls;
}

static void MantlePocketPy_RecordGcDebt(FMantleAllocHeader* header,
										unsigned long long bytes)
{
	if(header) MantlePocketPy_RecordGcDebtForCell(header->vm, header->cell, bytes);
}

void MantlePocketPy_InitQuotaCell(MantlePocketPy_QuotaCell* cell, int vm, int instance,
                                  unsigned long long generation, unsigned long long limit)
{
    if(!cell) return;
    cell->committed = 0;
    cell->reserved = 0;
    cell->limit = limit;
    cell->generation = generation;
    memset(cell->gc_debt, 0, sizeof(cell->gc_debt));
    cell->vm = vm;
    cell->instance = instance;
    cell->active = MantlePocketPy_ValidVm(vm) && generation != 0;
}

void MantlePocketPy_RetireQuotaCell(MantlePocketPy_QuotaCell* cell)
{
    if(cell) cell->active = 0;
}

void MantlePocketPy_RecordAllocationFatal(
    MantlePocketPy_AdmissionResult reason, int vm, int instance,
    unsigned long long generation)
{
    MantlePocketPy_FatalRecord* fatal;
    if(reason == MantlePocketPy_AdmissionOk || !MantlePocketPy_ValidVm(vm)) return;
    fatal = &MantlePocketPy_Fatal[vm];
    if(fatal->pending) return;
    fatal->generation = generation;
    fatal->vm = vm;
    fatal->instance = instance;
    fatal->reason = (int)reason;
    fatal->pending = 1;
}

int MantlePocketPy_TakeAllocationFatal(int vm, MantlePocketPy_FatalRecord* out)
{
    MantlePocketPy_FatalRecord* fatal;
    if(!out || !MantlePocketPy_ValidVm(vm)) return 0;
    fatal = &MantlePocketPy_Fatal[vm];
    if(!fatal->pending) return 0;
    *out = *fatal;
    fatal->generation = 0;
    fatal->vm = 0;
    fatal->instance = 0;
    fatal->reason = (int)MantlePocketPy_AdmissionOk;
    fatal->pending = 0;
    return 1;
}

int MantlePocketPy_HasAllocationFatal(int vm)
{
    return MantlePocketPy_ValidVm(vm) && MantlePocketPy_Fatal[vm].pending;
}

int MantlePocketPy_HasAllocationFatalForCell(
    int vm, const MantlePocketPy_QuotaCell* cell)
{
    const MantlePocketPy_FatalRecord* fatal;
    if(!MantlePocketPy_ValidVm(vm) || !cell || !cell->active || cell->vm != vm)
        return 0;
    fatal = &MantlePocketPy_Fatal[vm];
    return fatal->pending && fatal->vm == vm &&
           fatal->instance == cell->instance &&
           fatal->generation == cell->generation;
}

void MantlePocketPy_ResetAllocationFatal(int vm)
{
    if(!MantlePocketPy_ValidVm(vm)) return;
    MantlePocketPy_Fatal[vm].generation = 0;
    MantlePocketPy_Fatal[vm].vm = 0;
    MantlePocketPy_Fatal[vm].instance = 0;
    MantlePocketPy_Fatal[vm].reason = (int)MantlePocketPy_AdmissionOk;
    MantlePocketPy_Fatal[vm].pending = 0;
}

static void MantlePocketPy_RecordCellFatal(
    MantlePocketPy_AdmissionResult reason, int vm,
    const MantlePocketPy_QuotaCell* cell)
{
    const int owner_vm = cell ? cell->vm : vm;
    MantlePocketPy_RecordAllocationFatal(reason, owner_vm,
        cell ? cell->instance : 0, cell ? cell->generation : 0);
}

MantlePocketPy_AdmissionResult MantlePocketPy_TryReserve(
    int vm, MantlePocketPy_QuotaCell* cell, unsigned long long bytes,
    MantlePocketPy_Reservation* out)
{
    unsigned long long vm_total;
    unsigned long long cell_total = 0;
    MantlePocketPy_Reservation next;
    if(!out || !MantlePocketPy_ValidVm(vm)) return MantlePocketPy_AdmissionInvalid;
    if(cell && (!cell->active || cell->vm != vm || cell->generation == 0))
        return MantlePocketPy_AdmissionInvalid;

    if(MantlePocketPy_AddOverflow(MantlePocketPy_MemUsed[vm],
                                  MantlePocketPy_MemReserved[vm], &vm_total) ||
       MantlePocketPy_AddOverflow(vm_total, bytes, &vm_total))
        return MantlePocketPy_AdmissionSizeOverflow;
    if(MantlePocketPy_MemLimit[vm] && vm_total > MantlePocketPy_MemLimit[vm])
        return MantlePocketPy_AdmissionVmLimit;

    if(cell)
    {
        if(MantlePocketPy_AddOverflow(cell->committed, cell->reserved, &cell_total) ||
           MantlePocketPy_AddOverflow(cell_total, bytes, &cell_total))
            return MantlePocketPy_AdmissionSizeOverflow;
        if(cell->limit && cell_total > cell->limit)
            return MantlePocketPy_AdmissionInstanceLimit;
    }

    next.cell = cell;
    next.bytes = bytes;
    next.vm = vm;
    next.active = bytes != 0;
    if(bytes)
    {
        MantlePocketPy_MemReserved[vm] += bytes;
        if(cell) cell->reserved += bytes;
    }
    *out = next;
    return MantlePocketPy_AdmissionOk;
}

void MantlePocketPy_CommitReservation(MantlePocketPy_Reservation* reservation)
{
    MantlePocketPy_QuotaCell* cell;
    unsigned long long bytes;
    int vm;
    if(!reservation || !reservation->active) return;
    cell = reservation->cell;
    bytes = reservation->bytes;
    vm = reservation->vm;
    if(!MantlePocketPy_ValidVm(vm) || MantlePocketPy_MemReserved[vm] < bytes ||
       (cell && cell->reserved < bytes)) return;
    MantlePocketPy_MemReserved[vm] -= bytes;
    MantlePocketPy_MemUsed[vm] += bytes;
    if(cell)
    {
        cell->reserved -= bytes;
        cell->committed += bytes;
    }
    MantlePocketPy_ObserveAccounting();
	MantlePocketPy_RecordGcDebtForCell(vm, cell, bytes);
    reservation->active = 0;
}

void MantlePocketPy_RollbackReservation(MantlePocketPy_Reservation* reservation)
{
    MantlePocketPy_QuotaCell* cell;
    unsigned long long bytes;
    int vm;
    if(!reservation || !reservation->active) return;
    cell = reservation->cell;
    bytes = reservation->bytes;
    vm = reservation->vm;
    if(!MantlePocketPy_ValidVm(vm) || MantlePocketPy_MemReserved[vm] < bytes ||
       (cell && cell->reserved < bytes)) return;
    MantlePocketPy_MemReserved[vm] -= bytes;
    if(cell) cell->reserved -= bytes;
    reservation->active = 0;
}

static void MantlePocketPy_LegacyCharge(int vm, int instance, int add,
                                        unsigned long long bytes)
{
    if(!MantlePocketPy_InstanceCharge) return;
    while(bytes)
    {
        const unsigned long long chunk = bytes > (unsigned long long)LLONG_MAX
            ? (unsigned long long)LLONG_MAX : bytes;
        MantlePocketPy_InstanceCharge(vm, instance,
            add ? (long long)chunk : -(long long)chunk);
        bytes -= chunk;
    }
}

static void MantlePocketPy_ObserveAccounting(void)
{
#if MANTLE_POCKETPY_GC_DIAGNOSTICS
    MantlePocketPy_GcSweepStats* sweep =
        MantlePocketPy_CurrentVm >= 0 && MantlePocketPy_CurrentVm < MANTLE_POCKETPY_MAX_VM
            ? MantlePocketPy_GcActiveSweep[MantlePocketPy_CurrentVm] : NULL;
    if(sweep) ++sweep->accounting_calls;
#endif
}

static void MantlePocketPy_ChargeUnenforced(FMantleAllocHeader* header, int add,
                                            unsigned long long bytes)
{
    unsigned long long* vm_used;
    if(!header || !bytes || !MantlePocketPy_ValidVm(header->vm)) return;
    MantlePocketPy_ObserveAccounting();
    vm_used = &MantlePocketPy_MemUsed[header->vm];
    if(add)
    {
        *vm_used = ULLONG_MAX - *vm_used < bytes ? ULLONG_MAX : *vm_used + bytes;
        if(header->cell)
        {
            header->cell->committed = ULLONG_MAX - header->cell->committed < bytes
                ? ULLONG_MAX : header->cell->committed + bytes;
        }
        else if(!(header->flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE))
        {
            MantlePocketPy_LegacyCharge(header->vm, header->instance, 1, bytes);
        }
    }
    if(add) MantlePocketPy_RecordGcDebt(header, bytes);
    else
    {
        *vm_used = bytes > *vm_used ? 0 : *vm_used - bytes;
        if(header->cell)
        {
            header->cell->committed = bytes > header->cell->committed
                ? 0 : header->cell->committed - bytes;
        }
        else if(!(header->flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE))
        {
            MantlePocketPy_LegacyCharge(header->vm, header->instance, 0, bytes);
        }
    }
    if(MantlePocketPy_MemLimit[header->vm] &&
       MantlePocketPy_MemUsed[header->vm] > MantlePocketPy_MemLimit[header->vm])
        MantlePocketPy_MemOverBudget[header->vm] = 1;
}

static void MantlePocketPy_DebitAdmitted(FMantleAllocHeader* header,
                                         unsigned long long bytes)
{
    if(!header || !bytes || !MantlePocketPy_ValidVm(header->vm)) return;
    MantlePocketPy_ObserveAccounting();
    MantlePocketPy_MemUsed[header->vm] =
        bytes > MantlePocketPy_MemUsed[header->vm]
            ? 0 : MantlePocketPy_MemUsed[header->vm] - bytes;
    if(header->cell)
    {
        header->cell->committed = bytes > header->cell->committed
            ? 0 : header->cell->committed - bytes;
    }
}

MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedMalloc(
    size_t size, int vm, MantlePocketPy_QuotaCell* cell,
    MantlePocketPy_MallocFn allocate, void* context, void** out)
{
    size_t total;
    void* raw;
    FMantleAllocHeader* header;
    MantlePocketPy_Reservation reservation;
    MantlePocketPy_AdmissionResult result;
    if(!allocate || !out) return MantlePocketPy_AdmissionInvalid;
    result = MantlePocketPy_TotalSize(size, &total);
    if(result != MantlePocketPy_AdmissionOk)
    {
        MantlePocketPy_RecordCellFatal(result, vm, cell);
        return result;
    }
    result = MantlePocketPy_TryReserve(vm, cell, (unsigned long long)total, &reservation);
    if(result != MantlePocketPy_AdmissionOk)
    {
        MantlePocketPy_RecordCellFatal(result, vm, cell);
        return result;
    }
    raw = allocate(total, context);
    if(!raw)
    {
        MantlePocketPy_RollbackReservation(&reservation);
        MantlePocketPy_RecordCellFatal(MantlePocketPy_AdmissionAllocFailed, vm, cell);
        return MantlePocketPy_AdmissionAllocFailed;
    }
    header = (FMantleAllocHeader*)raw;
    header->size = (unsigned long long)total;
    header->cell = cell;
    header->vm = vm;
    header->instance = cell ? cell->instance : 0;
    header->flags = MANTLE_ALLOCATION_NO_LEGACY_CHARGE;
    MantlePocketPy_CommitReservation(&reservation);
    *out = (void*)(header + 1);
    return MantlePocketPy_AdmissionOk;
}

MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedMallocLike(
    const void* owner, size_t size, MantlePocketPy_MallocFn allocate,
    void* context, void** out)
{
    const FMantleAllocHeader* header;
    if(!owner || !allocate || !out) return MantlePocketPy_AdmissionInvalid;
    header = ((const FMantleAllocHeader*)owner) - 1;
    /* A numeric legacy owner is debited through the host callback. A new
     * admitted block cannot preserve that route, so never silently retarget it. */
    if(!header->cell && !(header->flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE))
    {
        MantlePocketPy_RecordAllocationFatal(MantlePocketPy_AdmissionInvalid,
            header->vm, header->instance, 0);
        return MantlePocketPy_AdmissionInvalid;
    }
    return MantlePocketPy_TryAdmittedMalloc(
        size, header->vm, header->cell, allocate, context, out);
}

MantlePocketPy_AdmissionResult MantlePocketPy_TryAdmittedRealloc(
    void* ptr, size_t size, MantlePocketPy_ReallocFn reallocate,
    void* context, void** out)
{
    FMantleAllocHeader* old_header;
    FMantleAllocHeader saved;
    FMantleAllocHeader* new_header;
    MantlePocketPy_Reservation reservation = {0};
    MantlePocketPy_AdmissionResult result;
    size_t new_total;
    void* raw;
    if(!ptr || size == 0 || !reallocate || !out)
        return MantlePocketPy_AdmissionInvalid;
    old_header = ((FMantleAllocHeader*)ptr) - 1;
    saved = *old_header;
    result = MantlePocketPy_TotalSize(size, &new_total);
    if(result != MantlePocketPy_AdmissionOk)
    {
        MantlePocketPy_RecordCellFatal(result, saved.vm, saved.cell);
        return result;
    }
    /* A legacy numeric-owner block was positively charged through the host
     * callback. Converting it in place would make its later debit bypass that
     * callback. Only already-admitted or stable-cell blocks may enter here. */
    if(!saved.cell && !(saved.flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE))
        return MantlePocketPy_AdmissionInvalid;
    if((unsigned long long)new_total > saved.size)
    {
        result = MantlePocketPy_TryReserve(saved.vm, saved.cell,
            (unsigned long long)new_total - saved.size, &reservation);
        if(result != MantlePocketPy_AdmissionOk)
        {
            MantlePocketPy_RecordCellFatal(result, saved.vm, saved.cell);
            return result;
        }
    }
    raw = reallocate(old_header, new_total, context);
    if(!raw)
    {
        MantlePocketPy_RollbackReservation(&reservation);
        MantlePocketPy_RecordCellFatal(MantlePocketPy_AdmissionAllocFailed,
            saved.vm, saved.cell);
        return MantlePocketPy_AdmissionAllocFailed;
    }
    new_header = (FMantleAllocHeader*)raw;
    if((unsigned long long)new_total > saved.size)
        MantlePocketPy_CommitReservation(&reservation);
    else if((unsigned long long)new_total < saved.size)
        MantlePocketPy_DebitAdmitted(&saved, saved.size - (unsigned long long)new_total);
    new_header->size = (unsigned long long)new_total;
    new_header->cell = saved.cell;
    new_header->vm = saved.vm;
    new_header->instance = saved.instance;
    new_header->flags = saved.flags | MANTLE_ALLOCATION_NO_LEGACY_CHARGE;
    *out = (void*)(new_header + 1);
    return MantlePocketPy_AdmissionOk;
}

int MantlePocketPy_GetAllocationOwner(const void* ptr, int* vm, int* instance,
                                      unsigned long long* generation,
                                      unsigned long long* total_bytes)
{
    const FMantleAllocHeader* header;
    if(!ptr) return 0;
    header = ((const FMantleAllocHeader*)ptr) - 1;
    if(vm) *vm = header->vm;
    if(instance) *instance = header->instance;
    if(generation) *generation = header->cell ? header->cell->generation : 0;
    if(total_bytes) *total_bytes = header->size;
    return 1;
}

int MantlePocketPy_DetachAllocationCell(
    void* ptr, MantlePocketPy_QuotaCell* expected_cell)
{
    FMantleAllocHeader* header;
    if(!ptr || !expected_cell) return 0;
    header = ((FMantleAllocHeader*)ptr) - 1;
    if(!(header->flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE) ||
       header->cell != expected_cell || header->vm != expected_cell->vm ||
       expected_cell->committed < header->size)
        return 0;
    expected_cell->committed -= header->size;
    header->cell = NULL;
    header->instance = 0;
    return 1;
}

void MantlePocketPy_FreeWith(void* ptr, MantlePocketPy_FreeFn release, void* context)
{
    FMantleAllocHeader* header;
#if MANTLE_POCKETPY_GC_DIAGNOSTICS
    MantlePocketPy_GcSweepStats* sweep;
    int category;
    int sample;
    MantlePocketPy_GcClockFn sample_clock;
    double charge_start;
    double charge_end;
#endif
    if(!ptr || !release) return;
    header = ((FMantleAllocHeader*)ptr) - 1;
#if MANTLE_POCKETPY_GC_DIAGNOSTICS
    sweep = MantlePocketPy_CurrentVm >= 0 && MantlePocketPy_CurrentVm < MANTLE_POCKETPY_MAX_VM
        ? MantlePocketPy_GcActiveSweep[MantlePocketPy_CurrentVm] : NULL;
    if(sweep) ++sweep->allocator_free_calls;
    category = sweep ? sweep->active_sample_category : -1;
    sample = category >= 0 && category < MantlePocketPy_SweepCategoryCount;
    sample_clock = sample ? MantlePocketPy_GcClock[MantlePocketPy_CurrentVm] : NULL;
    charge_start = sample_clock ? sample_clock() : 0;
#endif
    if(header->flags & MANTLE_ALLOCATION_NO_LEGACY_CHARGE)
        MantlePocketPy_DebitAdmitted(header, header->size);
    else
        MantlePocketPy_ChargeUnenforced(header, 0, header->size);
#if MANTLE_POCKETPY_GC_DIAGNOSTICS
    charge_end = sample_clock ? sample_clock() : 0;
#endif
    release(header, context);
#if MANTLE_POCKETPY_GC_DIAGNOSTICS
    if(sample_clock)
    {
        const double free_end = sample_clock();
        const double charge_seconds = charge_end - charge_start;
        const double free_seconds = free_end - charge_end;
        ++sweep->allocator_samples[category];
        sweep->charge_sample_seconds[category] += charge_seconds;
        sweep->free_sample_seconds[category] += free_seconds;
        if(charge_seconds > sweep->charge_sample_max_seconds[category])
            sweep->charge_sample_max_seconds[category] = charge_seconds;
        if(free_seconds > sweep->free_sample_max_seconds[category])
            sweep->free_sample_max_seconds[category] = free_seconds;
    }
#endif
}

static void* MantlePocketPy_LibcMalloc(size_t size, void* context)
{
    (void)context;
    return malloc(size);
}

static void* MantlePocketPy_LibcRealloc(void* ptr, size_t size, void* context)
{
    (void)context;
    return realloc(ptr, size);
}

static void MantlePocketPy_LibcFree(void* ptr, void* context)
{
    (void)context;
    free(ptr);
}

static MantlePocketPy_AllocResult MantlePocketPy_ToAllocResult(
    MantlePocketPy_AdmissionResult result)
{
    switch(result)
    {
        case MantlePocketPy_AdmissionOk: return MantlePocketPy_AllocOk;
        case MantlePocketPy_AdmissionInvalid: return MantlePocketPy_AllocInvalidSize;
        case MantlePocketPy_AdmissionSizeOverflow: return MantlePocketPy_AllocSizeOverflow;
        default: return MantlePocketPy_AllocFailed;
    }
}

MantlePocketPy_AllocResult MantlePocketPy_TryAdmittedVectorReserve(
    void** data, int* capacity, int elem_size, int requested_capacity,
    int vm, MantlePocketPy_QuotaCell* cell)
{
    size_t bytes;
    void* replacement = NULL;
    MantlePocketPy_AllocResult size_result;
    MantlePocketPy_AdmissionResult admission;
    if(!data || !capacity || elem_size <= 0 || *capacity < 0 || requested_capacity < 0)
        return MantlePocketPy_AllocInvalidSize;
    if(requested_capacity < 4) requested_capacity = 4;
    if(requested_capacity <= *capacity) return MantlePocketPy_AllocOk;
    size_result = MantlePocketPy_TryCountBytes(requested_capacity, elem_size, &bytes);
    if(size_result != MantlePocketPy_AllocOk)
    {
        if(size_result == MantlePocketPy_AllocSizeOverflow)
        {
            int owner_vm = vm;
            int owner_instance = cell ? cell->instance : 0;
            unsigned long long owner_generation = cell ? cell->generation : 0;
            if(*data) MantlePocketPy_GetAllocationOwner(*data, &owner_vm,
                &owner_instance, &owner_generation, NULL);
            MantlePocketPy_RecordAllocationFatal(MantlePocketPy_AdmissionSizeOverflow,
                owner_vm, owner_instance, owner_generation);
        }
        return size_result;
    }
    if(*data)
    {
        admission = MantlePocketPy_TryAdmittedRealloc(*data, bytes,
            MantlePocketPy_LibcRealloc, NULL, &replacement);
    }
    else
    {
        admission = MantlePocketPy_TryAdmittedMalloc(bytes, vm, cell,
            MantlePocketPy_LibcMalloc, NULL, &replacement);
    }
    if(admission != MantlePocketPy_AdmissionOk)
        return MantlePocketPy_ToAllocResult(admission);
    *data = replacement;
    *capacity = requested_capacity;
    return MantlePocketPy_AllocOk;
}

void* MantlePocketPy_Malloc(size_t size)
{
    size_t total;
    FMantleAllocHeader* header;
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
    if(MantlePocketPy_RootAuditEnabled) ++MantlePocketPy_RootAllocatorEntries[0];
#endif
    /* Default build stays non-denying: the remaining raw PK_MALLOC sites assign
     * straight into a struct field and dereference it with no NULL check, so
     * denying here would turn an over-budget script into a fault rather than a
     * terminal denial. MANTLE_POCKETPY_ENFORCE_QUOTA=1 opts a build into real
     * admission so the converted paths can be exercised end to end; it must not
     * default to 1 until every reachable caller propagates failure. */
#if MANTLE_POCKETPY_ENFORCE_QUOTA
    /* Only a live cell has a budget to enforce. With no active script cell the
     * block belongs to host/bootstrap work and must keep its legacy owner
     * attribution: admitted blocks are stamped NO_LEGACY_CHARGE and would stop
     * being debited through the host callback. */
    if(MantlePocketPy_CurrentQuotaCell)
    {
        void* admitted = NULL;
        if(MantlePocketPy_TryAdmittedMalloc(size, MantlePocketPy_CurrentVm,
               MantlePocketPy_CurrentQuotaCell, MantlePocketPy_LibcMalloc, NULL,
               &admitted) != MantlePocketPy_AdmissionOk)
            return NULL;
        return admitted;
    }
#endif
    if(MantlePocketPy_TotalSize(size, &total) != MantlePocketPy_AdmissionOk) return NULL;
    header = (FMantleAllocHeader*)MantlePocketPy_LibcMalloc(total, NULL);
    if(!header) return NULL;
    header->size = (unsigned long long)total;
    header->cell = MantlePocketPy_CurrentQuotaCell;
    header->vm = MantlePocketPy_CurrentVm;
    header->instance = header->cell ? header->cell->instance : MantlePocketPy_CurrentInstance;
    header->flags = header->cell ? MANTLE_ALLOCATION_NO_LEGACY_CHARGE : 0;
    MantlePocketPy_ChargeUnenforced(header, 1, header->size);
    return (void*)(header + 1);
}

void* MantlePocketPy_Realloc(void* ptr, size_t size)
{
    FMantleAllocHeader* old_header;
    FMantleAllocHeader saved;
    FMantleAllocHeader* new_header;
    size_t new_total;
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
    if(MantlePocketPy_RootAuditEnabled) ++MantlePocketPy_RootAllocatorEntries[1];
#endif
    if(!ptr) return MantlePocketPy_Malloc(size);
    if(size == 0)
    {
        MantlePocketPy_Free(ptr);
        return NULL;
    }
    if(MantlePocketPy_TotalSize(size, &new_total) != MantlePocketPy_AdmissionOk) return NULL;
    old_header = ((FMantleAllocHeader*)ptr) - 1;
#if MANTLE_POCKETPY_ENFORCE_QUOTA
    /* Same rule as Malloc, read off the block itself: only a cell-owned block
     * may grow through admission. A legacy numeric-owner block is debited via
     * the host callback and TryAdmittedRealloc refuses it by design. */
    if(old_header->cell)
    {
        void* admitted = NULL;
        if(MantlePocketPy_TryAdmittedRealloc(ptr, size, MantlePocketPy_LibcRealloc,
               NULL, &admitted) != MantlePocketPy_AdmissionOk)
            return NULL;
        return admitted;
    }
#endif
    saved = *old_header;
    new_header = (FMantleAllocHeader*)MantlePocketPy_LibcRealloc(old_header, new_total, NULL);
    if(!new_header) return NULL;
    if((unsigned long long)new_total > saved.size)
        MantlePocketPy_ChargeUnenforced(&saved, 1, (unsigned long long)new_total - saved.size);
    else if((unsigned long long)new_total < saved.size)
        MantlePocketPy_ChargeUnenforced(&saved, 0, saved.size - (unsigned long long)new_total);
    new_header->size = (unsigned long long)new_total;
    new_header->cell = saved.cell;
    new_header->vm = saved.vm;
    new_header->instance = saved.instance;
    new_header->flags = saved.flags;
    return (void*)(new_header + 1);
}

void MantlePocketPy_Free(void* ptr)
{
#if MANTLE_POCKETPY_ROOT_DIAGNOSTICS
    if(MantlePocketPy_RootAuditEnabled) ++MantlePocketPy_RootAllocatorEntries[2];
#endif
    MantlePocketPy_FreeWith(ptr, MantlePocketPy_LibcFree, NULL);
}
