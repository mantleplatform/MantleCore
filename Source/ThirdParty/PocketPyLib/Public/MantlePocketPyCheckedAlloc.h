// Copyright Joshua Eger. All Rights Reserved.
/* #153: fallible allocation foundations, not a hard memory cap. These helpers do
 * not set Python exceptions, run GC, change quota policy, or unwind the VM. */
#pragma once

#include "MantlePocketPyHooks.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MantlePocketPy_AllocResult {
    MantlePocketPy_AllocOk = 0,
    MantlePocketPy_AllocInvalidSize,
    MantlePocketPy_AllocSizeOverflow,
    MantlePocketPy_AllocFailed
} MantlePocketPy_AllocResult;

/* Output is unchanged on failure. Zero factors/counts are valid; element size
 * must be positive. Check signed arithmetic before passing a computed count. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TrySizeAdd(
    size_t a, size_t b, size_t* out);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TrySizeMultiply(
    size_t a, size_t b, size_t* out);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryCountBytes(
    int count, int elem_size, size_t* out);

/* Same ownership contract as realloc for a nonzero size: failure must preserve
 * the original block, success preserves its prefix and may move it. Must not
 * run GC, reenter the vector or modify its fields. Context makes bounded failure tests
 * local to a call; there is no process-wide allocator override/test switch. */
typedef void* (*MantlePocketPy_ReallocateFn)(void* ptr, size_t size, void* context);

/* PocketPy vectors reserve at least four elements and never shrink here.
 * Invalid input, overflow or allocation failure leaves data/capacity unchanged.
 * Length is deliberately not modified. Existing contents are preserved under
 * the callback's realloc contract. No callback is made for invalid/no-op input.
 * The caller must supply a valid existing vector: capacity describes the live
 * allocation at data (NULL for an empty vector). This cannot inspect its size.
 * This does not check a caller's prior length/growth arithmetic or add allocator
 * headers; those contracts must be propagated separately before quota denial. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorReserve(
    void** data, int* capacity, int elem_size, int requested_capacity,
    MantlePocketPy_ReallocateFn reallocate, void* context);

/* Quota-admitted counterpart for a future converted PocketPy caller. Empty
 * vectors use vm/cell for initial ownership; existing allocations always retain
 * the exact owner in their private header. Quota/backing failure is published to
 * the fixed fatal channel by the admitted allocator. No-op/invalid input does
 * not allocate or publish a fault. Failure leaves data/capacity/bytes unchanged.
 * Existing storage must already be admitted (a legacy numeric-owner block is
 * rejected unchanged). This API is intentionally not wired into legacy
 * c11_vector wrappers, whose callers still abort or require an allocating
 * Python exception on failure. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryAdmittedVectorReserve(
    void** data, int* capacity, int elem_size, int requested_capacity,
    int vm, MantlePocketPy_QuotaCell* cell);

typedef struct MantlePocketPy_AppendPlan {
    int length;
    int capacity;
    size_t destination_offset;
    size_t copy_bytes;
} MantlePocketPy_AppendPlan;

/* Preserve upstream doubling below 1024 and 25-percent growth thereafter.
 * Neither function allocates; every output remains unchanged on failure. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryNextCapacity(
    int capacity, int* out);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryPlanAppend(
    int length, int capacity, int elem_size, int count, MantlePocketPy_AppendPlan* out);

/* On failure data/length/capacity and original contents remain unchanged.
 * Source must remain valid through reallocation and not overlap the appended
 * destination. Use the self variant for a slice of the vector being grown:
 * source_offset/count refer to its original logical elements, not byte offsets.
 * Count zero is a no-op; negative counts are rejected by these fallible APIs.
 * Existing vector and allocator preconditions above still apply. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorExtend(
    void** data, int* length, int* capacity, int elem_size, const void* source, int count,
    MantlePocketPy_ReallocateFn reallocate, void* context);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorExtendSelf(
    void** data, int* length, int* capacity, int elem_size, int source_offset, int count,
    MantlePocketPy_ReallocateFn reallocate, void* context);

/* Insert source is a stable, external, already-materialized element, not a
 * pointer into the vector. Index is in [0, length]. All fallible work precedes
 * memmove; failure leaves fields and original bytes unchanged. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorInsert(
    void** data, int* length, int* capacity, int elem_size, int index, const void* source,
    MantlePocketPy_ReallocateFn reallocate, void* context);

/* Emplace publishes one UNINITIALIZED slot on success, matching legacy callers.
 * Caller must initialize it before GC/reentry; fallible construction needs its
 * own cleanup contract. Output must not alias vector metadata. Output and vector
 * remain unchanged on failure. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorEmplace(
    void** data, int* length, int* capacity, int elem_size, void** out_slot,
    MantlePocketPy_ReallocateFn reallocate, void* context);

/* Copy into output storage that owns no allocation (and does not alias source
 * metadata). Retain source capacity/minimum-four policy. Outputs unchanged on
 * failure; successful output owns the new allocation. Source must remain valid
 * through allocation; no GC/reentry is permitted by these operation contracts. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryVectorCopy(
    const void* source, int length, int capacity, int elem_size,
    void** out_data, int* out_length, int* out_capacity,
    MantlePocketPy_ReallocateFn reallocate, void* context);

/* VectorType has data/length/capacity/elem_size fields; T matches elem_size.
 * Capture vector, index, value exactly once, in that order, before mutation.
 * Typed locals preserve alignment and values referenced inside the old vector.
 * Allocator must not run GC/reenter: a copied TValue is not an added GC root.
 * On failure ownership of captured values stays with the caller. Expressions
 * that transfer ownership need caller cleanup before recoverable use. The result
 * lvalue must be external, stable storage, not an expression in the vector.
 * Internal mantle_pp_ names are reserved within these macro expansions. */
#define MANTLE_POCKETPY_TRY_VECTOR_PUSH(T, VectorType, vector, value, reallocate, context, result) \
    do { \
        VectorType* mantle_pp_vector = (vector); \
        T mantle_pp_value = (value); \
        (result) = MantlePocketPy_TryVectorExtend( \
            &mantle_pp_vector->data, &mantle_pp_vector->length, &mantle_pp_vector->capacity, \
            mantle_pp_vector->elem_size, &mantle_pp_value, 1, (reallocate), (context)); \
    } while (0)

#define MANTLE_POCKETPY_TRY_VECTOR_INSERT(T, VectorType, vector, index, value, reallocate, context, result) \
    do { \
        VectorType* mantle_pp_vector = (vector); \
        int mantle_pp_index = (index); \
        T mantle_pp_value = (value); \
        (result) = MantlePocketPy_TryVectorInsert( \
            &mantle_pp_vector->data, &mantle_pp_vector->length, &mantle_pp_vector->capacity, \
            mantle_pp_vector->elem_size, mantle_pp_index, &mantle_pp_value, (reallocate), (context)); \
    } while (0)

/* Pure representational preflight, not an allocation/quota guarantee. Payload
 * overhead includes every header and terminator added before the int-sized heap
 * request. Repeat counts are checked while still wide; nonpositive/empty repeats
 * yield zero without negating INT64_MIN. Outputs are unchanged on failure. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryPayloadLimit(
    size_t overhead, int* out);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TrySequenceAdd(
    int left, int right, int maximum, int* out);
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryRepeatLength(
    int length, int64_t repeats, int maximum, int* out);

typedef struct MantlePocketPy_RepeatPlan {
    int length;
    int capacity;
} MantlePocketPy_RepeatPlan;

/* Models the existing repeated-append growth schedule at growth boundaries,
 * without allocating or iterating once per repetition. Production still uses
 * its original incremental appends, not a single exact-capacity allocation. */
POCKETPY_HOOKS_API MantlePocketPy_AllocResult MantlePocketPy_TryPlanRepeat(
    int item_count, int64_t repeats, int elem_size, MantlePocketPy_RepeatPlan* out);

#ifdef __cplusplus
}
#endif
