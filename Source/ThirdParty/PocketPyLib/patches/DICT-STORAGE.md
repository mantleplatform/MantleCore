# Dictionary storage co-allocation (#158)

This is a representation change to reduce real allocation/destruction work. It
does not change GC cadence, thresholds, CPU attribution, memory limits, allocator
backend, workload, or the fatal failure contract of legacy callers. Performance
is unmeasured until the unchanged full50 fixture is run on the integrated build.

## Layout and ownership

`Dict` and `PyObject` retain their exact layouts. `entries.data` owns one allocation:
the existing entry capacity first, then the existing short/long index table.
`indices` points into this allocation, never into the Dict object or an arena.
Index capacities, widths, load thresholds and entry-capacity progression are
unchanged. Generic vector growth/copy must not consume this storage; only generic
clear and destruction (one free of the allocation base) remain valid.

On current x64 targets a TValue is 24 bytes (including its 16-byte union), a
DictEntry is 56 bytes, Dict is 48 bytes, and PyObject's
header is 8 bytes. An exact dict remains in the 64-byte pool class. Its initial
four entries and 17 short indices previously used (224+16)+(34+16)=290 accounted
bytes outside the pool; now they use 224+34+16=274. This saves one physical
allocation and one actual allocator header, not an omitted or deferred charge.
The code derives these sizes and obtains the actual allocator-header size through
a pure C query, checking all sums/products and signed charge range before allocation.

Both regions were allocated by the same current owner during construction,
rehash and copy. They now share that original-owner header. Growing under a
different current script preserves the old header through the existing realloc
hook; the delta is still exactly the increase in entry capacity. Rehash creates
a fresh allocation owned by its current script and frees the old owner's block,
as previously occurred separately for both regions. Free always charges the
stored owner immediately. No extra pooled-object ownership assumption is added.

For entry capacity E, entry size S, index bytes I and header H, steady storage is
E*S+I+H versus E*S+I+2H. Rehash keeps old and new storage alive together before
releasing the old allocation, so both contribute to the transient peak. A moving
libc realloc may internally retain old and new allocations. For entry growth
without rehash, the old layout's possible peak was old_entries + new_entries +
I + 3H; the new possible peak is old_entries + new_entries + 2I + 2H. Thus a
moving realloc can transiently use I-H more bytes despite lower steady storage,
and must move the index tail as additional work. The allocator's logical charge
still reports only the net entry-capacity delta, as before; no physical realloc
peak tracking is claimed. Allocator
usable-size/fragmentation and arena amortization are not included in these sums.

## Mutation and lifetime contract

The fallible constructor/copy build a local result and publish it only after
allocation and initialization. Emplace checks the original growth plan, reallocates
once if needed, derives pointers from the returned base, then memmoves the old
index tail to its new offset before publishing an uninitialized entry. Large
index tails can overlap during movement. Failure changes neither metadata, data
nor output slot. The callback cannot collect, reenter or mutate the dictionary.
Production wrappers retain fatal allocation/size failure; no quota denial or
recoverable VM propagation is enabled.

Ordinary key/value references and private-root values are copied before growth.
Rehash builds a separate dictionary, copies live entries in order without hashing
again, publishes it, then destroys the shallow old ownership record. Its capacity
already fits every old entry, so copying cannot request further allocation.
Copy retains tombstones/capacity and copies only initialized entries plus indices.
Clear and stable compaction retain the allocation. The existing iterator snapshot
and `memcmp(sizeof(Dict))` are untouched: no inline bytes, added fields or new
padding are compared. Reallocation moves index pointers only on the same entry
capacity changes that already invalidated iterators.

## Regression coverage and verification

- A bounded local allocator denies individual calls, moves blocks and poisons
  their old bytes. Both index widths, overlapping-tail movement, no-growth paths,
  independent copies, failed constructor/copy/emplace and pure size bounds are
  checked using production helpers. No global allocator override is installed.
- The existing private-root collision/compaction/no-allocation test now supplies
  contiguous aligned stack storage for both synthetic index widths.
- An initialized host checks actual allocator and host per-owner/VM balances at
  seven checkpoints, including foreign-current-owner grow/free, fresh-owner
  rehash, independent copy and clear. Three physical storage lifetimes require
  three mallocs/frees rather than six. Real native interior key/value references
  cross growth and rehash.
- Initialized Python covers cross-script mutation through a catalog module,
  growth, deletion/compaction, copy/clear/reuse, iterator update versus structural
  invalidation, subclasses, shallow value identity and explicit GC.
- Collector ABI coverage now requires exactly one combined-storage free per
  sampled exact dictionary destructor instead of two.

Root must run focused PythonSandbox tests, full automation, Shipping and Linux
builds, followed by unchanged full50 with the same opt-in measurements. No tests,
builds, workloads or generators were run while authoring this patch. Fewer frees
do not establish a latency improvement, and shared GC CPU attribution remains
an independent unsolved requirement of #158.
