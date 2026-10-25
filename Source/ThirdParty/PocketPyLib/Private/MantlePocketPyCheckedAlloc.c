// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePocketPyCheckedAlloc.h"
#include <limits.h>
#include <string.h>

MantlePocketPy_AllocResult MantlePocketPy_TrySizeAdd(size_t a, size_t b, size_t* out)
{
    if (!out) return MantlePocketPy_AllocInvalidSize;
    if (b > (size_t)-1 - a) return MantlePocketPy_AllocSizeOverflow;
    *out = a + b;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TrySizeMultiply(size_t a, size_t b, size_t* out)
{
    if (!out) return MantlePocketPy_AllocInvalidSize;
    if (a != 0 && b > (size_t)-1 / a) return MantlePocketPy_AllocSizeOverflow;
    *out = a * b;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryCountBytes(int count, int elem_size, size_t* out)
{
    if (count < 0 || elem_size <= 0) return MantlePocketPy_AllocInvalidSize;
    return MantlePocketPy_TrySizeMultiply((size_t)count, (size_t)elem_size, out);
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorReserve(
    void** data, int* capacity, int elem_size, int requested_capacity,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!data || !capacity || !reallocate || elem_size <= 0 ||
        *capacity < 0 || requested_capacity < 0) return MantlePocketPy_AllocInvalidSize;
    if (requested_capacity < 4) requested_capacity = 4;
    if (requested_capacity <= *capacity) return MantlePocketPy_AllocOk;

    size_t bytes;
    MantlePocketPy_AllocResult result =
        MantlePocketPy_TryCountBytes(requested_capacity, elem_size, &bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    void* replacement = reallocate(*data, bytes, context);
    if (!replacement) return MantlePocketPy_AllocFailed;
    *data = replacement;
    *capacity = requested_capacity;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryNextCapacity(int capacity, int* out)
{
    if (capacity < 0 || !out) return MantlePocketPy_AllocInvalidSize;
    size_t next = (size_t)capacity + (capacity < 1024 ? (size_t)capacity : (size_t)(capacity >> 2));
    if (next > INT_MAX) return MantlePocketPy_AllocSizeOverflow;
    *out = (int)next;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryPlanAppend(
    int length, int capacity, int elem_size, int count, MantlePocketPy_AppendPlan* out)
{
    if (!out || length < 0 || capacity < length || elem_size <= 0 || count < 0) {
        return MantlePocketPy_AllocInvalidSize;
    }
    if (count > INT_MAX - length) return MantlePocketPy_AllocSizeOverflow;
    MantlePocketPy_AppendPlan plan;
    plan.length = length + count;
    plan.capacity = capacity;
    MantlePocketPy_AllocResult result = MantlePocketPy_TryCountBytes(length, elem_size, &plan.destination_offset);
    if (result != MantlePocketPy_AllocOk) return result;
    result = MantlePocketPy_TryCountBytes(count, elem_size, &plan.copy_bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    size_t total_bytes;
    result = MantlePocketPy_TrySizeAdd(plan.destination_offset, plan.copy_bytes, &total_bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    if (plan.length > capacity) {
        result = MantlePocketPy_TryNextCapacity(capacity, &plan.capacity);
        if (result != MantlePocketPy_AllocOk) return result;
        if (plan.capacity < plan.length) plan.capacity = plan.length;
        if (plan.capacity < 4) plan.capacity = 4;
        result = MantlePocketPy_TryCountBytes(plan.capacity, elem_size, &total_bytes);
        if (result != MantlePocketPy_AllocOk) return result;
    }
    *out = plan;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorExtend(
    void** data, int* length, int* capacity, int elem_size, const void* source, int count,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!data || !length || !capacity || !reallocate || (count > 0 && !source)) {
        return MantlePocketPy_AllocInvalidSize;
    }
    MantlePocketPy_AppendPlan plan;
    MantlePocketPy_AllocResult result =
        MantlePocketPy_TryPlanAppend(*length, *capacity, elem_size, count, &plan);
    if (result != MantlePocketPy_AllocOk || count == 0) return result;
    if (plan.capacity > *capacity) {
        result = MantlePocketPy_TryVectorReserve(data, capacity, elem_size, plan.capacity, reallocate, context);
        if (result != MantlePocketPy_AllocOk) return result;
    }
    memcpy((char*)*data + plan.destination_offset, source, plan.copy_bytes);
    *length = plan.length;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorExtendSelf(
    void** data, int* length, int* capacity, int elem_size, int source_offset, int count,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!data || !length || !capacity || !reallocate || source_offset < 0 ||
        source_offset > *length || count < 0 || count > *length - source_offset || (count > 0 && !*data)) {
        return MantlePocketPy_AllocInvalidSize;
    }
    MantlePocketPy_AppendPlan plan;
    MantlePocketPy_AllocResult result =
        MantlePocketPy_TryPlanAppend(*length, *capacity, elem_size, count, &plan);
    if (result != MantlePocketPy_AllocOk || count == 0) return result;
    size_t source_bytes;
    result = MantlePocketPy_TryCountBytes(source_offset, elem_size, &source_bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    if (plan.capacity > *capacity) {
        result = MantlePocketPy_TryVectorReserve(data, capacity, elem_size, plan.capacity, reallocate, context);
        if (result != MantlePocketPy_AllocOk) return result;
    }
    // The source ends at/before the old logical length, where the destination
    // begins. Rebase after reserve; the valid ranges cannot overlap.
    memcpy((char*)*data + plan.destination_offset, (char*)*data + source_bytes, plan.copy_bytes);
    *length = plan.length;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorInsert(
    void** data, int* length, int* capacity, int elem_size, int index, const void* source,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!data || !length || !capacity || !source || !reallocate || index < 0 || index > *length) {
        return MantlePocketPy_AllocInvalidSize;
    }
    MantlePocketPy_AppendPlan plan;
    MantlePocketPy_AllocResult result =
        MantlePocketPy_TryPlanAppend(*length, *capacity, elem_size, 1, &plan);
    if (result != MantlePocketPy_AllocOk) return result;
    size_t offset, tail_bytes;
    result = MantlePocketPy_TryCountBytes(index, elem_size, &offset);
    if (result != MantlePocketPy_AllocOk) return result;
    result = MantlePocketPy_TryCountBytes(*length - index, elem_size, &tail_bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    if (plan.capacity > *capacity) {
        result = MantlePocketPy_TryVectorReserve(data, capacity, elem_size, plan.capacity, reallocate, context);
        if (result != MantlePocketPy_AllocOk) return result;
    }
    char* destination = (char*)*data + offset;
    memmove(destination + plan.copy_bytes, destination, tail_bytes);
    memcpy(destination, source, plan.copy_bytes);
    *length = plan.length;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorEmplace(
    void** data, int* length, int* capacity, int elem_size, void** out_slot,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!data || !length || !capacity || !out_slot || !reallocate) return MantlePocketPy_AllocInvalidSize;
    MantlePocketPy_AppendPlan plan;
    MantlePocketPy_AllocResult result =
        MantlePocketPy_TryPlanAppend(*length, *capacity, elem_size, 1, &plan);
    if (result != MantlePocketPy_AllocOk) return result;
    if (plan.capacity > *capacity) {
        result = MantlePocketPy_TryVectorReserve(data, capacity, elem_size, plan.capacity, reallocate, context);
        if (result != MantlePocketPy_AllocOk) return result;
    }
    *out_slot = (char*)*data + plan.destination_offset;
    *length = plan.length;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryVectorCopy(
    const void* source, int length, int capacity, int elem_size,
    void** out_data, int* out_length, int* out_capacity,
    MantlePocketPy_ReallocateFn reallocate, void* context)
{
    if (!out_data || !out_length || !out_capacity || !reallocate || length < 0 ||
        capacity < length || (length > 0 && !source)) return MantlePocketPy_AllocInvalidSize;
    size_t bytes;
    MantlePocketPy_AllocResult result = MantlePocketPy_TryCountBytes(length, elem_size, &bytes);
    if (result != MantlePocketPy_AllocOk) return result;
    void* replacement = NULL;
    int replacement_capacity = 0;
    result = MantlePocketPy_TryVectorReserve(&replacement, &replacement_capacity, elem_size, capacity, reallocate, context);
    if (result != MantlePocketPy_AllocOk) return result;
    if (bytes) memcpy(replacement, source, bytes);
    *out_data = replacement;
    *out_length = length;
    *out_capacity = replacement_capacity;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryPayloadLimit(size_t overhead, int* out)
{
    if (!out) return MantlePocketPy_AllocInvalidSize;
    if (overhead > INT_MAX) return MantlePocketPy_AllocSizeOverflow;
    *out = (int)((size_t)INT_MAX - overhead);
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TrySequenceAdd(int left, int right, int maximum, int* out)
{
    if (!out || left < 0 || right < 0 || maximum < 0) return MantlePocketPy_AllocInvalidSize;
    if (left > maximum || right > maximum - left) return MantlePocketPy_AllocSizeOverflow;
    *out = left + right;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryRepeatLength(int length, int64_t repeats, int maximum, int* out)
{
    if (!out || length < 0 || maximum < 0) return MantlePocketPy_AllocInvalidSize;
    if (length == 0 || repeats <= 0) {
        *out = 0;
        return MantlePocketPy_AllocOk;
    }
    if (repeats > maximum / length) return MantlePocketPy_AllocSizeOverflow;
    *out = length * (int)repeats;
    return MantlePocketPy_AllocOk;
}

MantlePocketPy_AllocResult MantlePocketPy_TryPlanRepeat(
    int item_count, int64_t repeats, int elem_size, MantlePocketPy_RepeatPlan* out)
{
    if (!out || elem_size <= 0) return MantlePocketPy_AllocInvalidSize;
    MantlePocketPy_RepeatPlan plan;
    MantlePocketPy_AllocResult result = MantlePocketPy_TryRepeatLength(item_count, repeats, INT_MAX, &plan.length);
    if (result != MantlePocketPy_AllocOk) return result;
    plan.capacity = 0;
    while (plan.capacity < plan.length) {
        // Every preceding append that fits leaves capacity unchanged. Jump to
        // the last such logical length, then plan the first append needing growth.
        int prefix = plan.capacity - plan.capacity % item_count;
        MantlePocketPy_AppendPlan next;
        result = MantlePocketPy_TryPlanAppend(prefix, plan.capacity, elem_size, item_count, &next);
        if (result != MantlePocketPy_AllocOk) return result;
        plan.capacity = next.capacity;
    }
    *out = plan;
    return MantlePocketPy_AllocOk;
}
