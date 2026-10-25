# Optional collector diagnostics (#158)

## Optional current-thread counters

`-MantleScriptBenchGcThreadCounters` additionally requires the existing metrics
gate. It adds binding-only Windows `GetThreadTimes` user/kernel counters in raw
100 ns units and `QueryThreadCycleTime` raw cycles at whole-collection boundaries.
The flag works with either detail or totals-only mode; it never enables metrics
or changes collection, allocator behavior, watchdogs, budgets or CPU attribution.
Shipping excludes its parsing, sampler, storage and receipts. Other platforms
report unavailable pairs instead of claiming measured zero execution.

The original wall start occurs before the additional begin queries; the original
wall end occurs after end queries and POD accumulation. Thus query overhead stays
in the reported wall interval and unchanged charged outer resume. Counter
intervals are slightly interior to wall boundaries. Flag-off adds no OS queries
or clock reads and retains the old wall-clock positions and receipt contract;
the compiled conditional branches are not a claim of identical machine timings.
No sampling occurs per object and no C observer ABI/collector patch is added.

Outside the charged interval, `MANTLE_BENCH_PY_GC_THREAD` uses the same
context/kind/id/path/count and occurrence as its immediately preceding whole-GC
receipt. `pairs` counts completed observations; `valid_cpu_pairs` and
`valid_cycle_pairs` independently count successful nonregressing pairs.
`invalid_pairs` counts their union, not their sum. `cpu_regressions` rejects a
decrease in either user or kernel counter, and `cycle_regressions` rejects a raw
cycle decrease. Only valid corresponding pairs contribute to `user_100ns`,
`kernel_100ns`, and `raw_cycles`. Valid zero deltas remain valid; a zero with zero
valid pairs is unavailable. The `source` is `windows_current_thread` or
`unavailable`, and `diagnostic_mode` retains detail/totals_only. Counts and sums
cover all collections of that kind in the outer resume, not a maximum pause.
Reporting, Configure, and VM Close/reset clear counter state.

100 ns is a unit, not guaranteed effective resolution. Raw cycles must not be
converted to time. Neither wall-minus-CPU nor cycles establish exact off-CPU
duration, per-script collector ownership, a speedup, or a language comparison.
Quantization, query-boundary skew, clock-frequency/migration effects and opt-in
overhead must remain visible. Compare paired unchanged totals-only runs with
the flag off/on, preserving startup failures and pre-fault object populations.

`GcThreadCounterPairValidation` exercises real accumulator logic with valid
zero, partial failure and independent regressing inputs; it installs no runtime
sampler override. `GcThreadCountersInitializedWiringAndReset` exercises production
command-line parsing/metrics gates, actual explicit collections, detail/totals
receipts and RestartAll. Its automatic-collector setup creates a bounded number
of native unreachable empty lists above the existing maximum GC threshold and
calls the real `gc.collect_hint`; it changes no threshold and labels this
uncharged native test context `native_test`, never a benchmark/resume result.
No test asserts a duration, cycle rate, or wall-minus-CPU threshold.

`MANTLE_POCKETPY_GC_DIAGNOSTICS` is defined to zero for Shipping by PocketPyLib.Build.cs.
The C callback declarations/storage/calls and binding measurements/logging are absent there.
In other builds, Mantle installs observers only for the existing
`-MantleScriptBenchMetrics` flag. Collection thresholds, roots, tracing, sweep, watchdogs,
memory/CPU budgets and charged elapsed time are unchanged.

## Totals-only comparison mode

Detailed observations remain the default. Add
`-MantleScriptBenchGcTotalsOnly` together with `-MantleScriptBenchMetrics` to
retain only the existing whole-collection observer and its two clock reads.
The host's existing benchmark/build validation still controls whether any
observer is installed; the new switch alone does nothing. Shipping excludes
the switch parsing, mode storage, observer wiring and reports entirely.

Whole-GC receipts append `diagnostic_mode=detail` or
`diagnostic_mode=totals_only`. Totals-only mode leaves phase/sweep observers,
sweep clock and handle-category callback null. It emits no `GC_PHASES`,
`GC_SWEEP` or `GC_DTOR_SAMPLE` receipts; missing measurements are not zeros.
Automatic and explicit collections keep the same timing, reclaimed count,
reporting boundary and complete elapsed CPU charge. The mode never forces a
collection or changes when one happens.

Compare unchanged full50 runs on the same build, arguments and fixture with
only this additional switch differing. Compare whole-GC and resume timings
alongside actual freed counts and outcomes. Development still contains inactive
diagnostic branches and stack POD initialization, unlike Shipping. Therefore
this comparison estimates enabled-detail overhead, not uninstrumented Shipping
latency, and cannot by itself establish a production performance improvement.

`GcTotalsOnlyWiringAndReceipts` runs real initialized collections with metrics
disabled, detail enabled, and totals-only enabled. It captures native receipts,
proves the detail clock actually ran, then verifies the same whole observer is
retained while detail clocks/records disappear. Reset clears the old observers.
No collection cost is subtracted from budget accounting.

The existing whole-collection `MantlePocketPy_GcObserver` contract and
`MANTLE_BENCH_PY_GC` marker retain their meanings. An additive phase observer surrounds
four actual source boundaries, in order:

1. `roots`: ManagedHeap__mark scans VM roots, including native gc_mark roots. The work count
   is the number of objects queued after root scanning, not the number of root references.
2. `graph`: the existing mark work-stack loop visits reachable objects. The work count is
   the number of objects popped; no clock read occurs per object.
3. `small_sweep`: MultiPool__sweep_dealloc, including native destructors/finalizers. Its
   reclaimed-object count is reported; no small-pool scanning count is inferred.
4. `large_sweep`: the large-object loop and list compaction, including native destructors
   and frees. Reports objects inspected and reclaimed.

The binding observer reads a monotonic clock and updates fixed-size POD arrays only. It
does not allocate, call Python, log, or dispatch host callbacks during collection. Native
finalizer time is already inside sweep, not an extra additive phase. There are eight phase
boundary observations per collection, plus the pre-existing whole-collection observations.
No callbacks are installed while trusted VM bootstrap runs. Close clears all callbacks
before py_resetvm and slot reuse. The initialized sandbox removes upstream script GC debug
callbacks, so these observers inherit the existing non-reentrant whole-collector contract.

`ReportBenchmarkGc`, already outside the charged interval, emits one additive record for each
explicit/automatic kind that collected during the outer resume:

```text
MANTLE_BENCH_PY_GC_PHASES context=resume kind=automatic id=40 path=Server/Example.py count=1 root_ms=... graph_ms=... small_sweep_ms=... large_sweep_ms=... unclassified_ms=... root_max_ms=... graph_max_ms=... small_sweep_max_ms=... large_sweep_max_ms=... root_queued=... graph_objects=... small_freed=... large_scanned=... large_freed=...
```

Times without `max` are sums across the corresponding whole marker's `count` collections.
Phase maxima are individual-phase maxima and need not belong to the same collection; do
not sum them to claim the maximum total pause. `unclassified_ms` is whole elapsed time minus
the four phase sums: collector bookkeeping and observer boundary overhead remain visible.
Small plus large freed counts must equal the whole marker's freed count. Phase sums plus
unclassified must reconcile with whole total (allow printed rounding). Match context/kind/
instance/path and log occurrence; this is not a global collection ID or allocator-owner ledger.

The initialized automation `CollectorPhaseObserversPartitionActualCollection` temporarily
installs fixed-size test observers only on its own scratch-world slot. It checks boundary
order/balance, explicit collection count, root/graph work, small/large reclamation, total
freed reconciliation, duration containment, retained handle liveness and observer removal
on reset. It tolerates additional natural collections without changing thresholds. The
test does not establish timing overhead, performance, or fair CPU ownership.

Root verification: focused observer test and PythonSandbox suite, Shipping compile to verify
exclusion, then unchanged mixed/full50 diagnostics with the existing flag and no forced GC.
Compare whole-collection totals with prior instrumentation before interpreting a latency
change. This patch measures #158; it does not fix shared-GC attribution or nonincremental pauses.

## Small-sweep detail follow-up

The additive `MantlePocketPy_GcSweepObserver` receives a stack-owned, fixed-size POD record
once per small sweep, before the existing SmallSweep end observation. A separate clock
pointer uses the same monotonic clock as the binding. Both are opt-in and Shipping-excluded.
The active record pointer is cleared before delivery; Close clears the observer, clock,
handle-finalizer identity and active pointer before destroying/reusing a VM. No tracing
state retains Python objects. Collector reentrancy remains unsupported as before.

Exact counters cover pools and arenas visited, all blocks scanned, already-free blocks,
marked live blocks, dead blocks, arenas already wholly empty at entry (and their block count),
and arenas released.
Dead objects are categorized by exact builtin type (list/dict/string/tuple), the installed
native host-handle destructor identity, or other. Builtin subclasses fall under other.
The categories sum to dead_blocks, which equals SmallSweep's freed count. Every scanned
block is exactly one of free/live/dead. These are counts, not allocated byte attribution.

`allocator_free_calls` counts non-null PK_FREE calls during small sweep, including container
backing storage and released arenas. `accounting_calls` counts actual InstanceCharge callback
invocations during that interval, including any other allocation/reallocation if present.
These count the current collection even though normal accounting still uses each allocation's
original VM/instance header. No accounting is deferred, combined, removed or reassigned.

Each of five size-class pools takes three clock reads: before scan/destruction, at the start
of arena partition/release, and after partition/release. `scan_destroy_ms` includes every
block check, free-list rebuild, destructor, native finalizer and destructor sampling overhead.
`partition_release_ms` includes arena partitioning, allocator/accounting work for arena release,
and available_index update. Both are nested subsets of SmallSweep, not additional GC charges.

Destructor sampling uses category-local dead ordinals 1, 257, 513, ... (cadence 256), reset
for each collection. Only sampled objects take two extra clock reads surrounding the actual
PyObject__dtor, including any backing frees/accounting and native finalizer. Thus a collection
with N dead objects in one category has ceil(N/256) samples. Aggregated records sum sample
counts from each collection separately. The reported sampled_subset_ms is ONLY the elapsed
time of those actual calls: do not multiply it into an exact destructor total, subtract it
to claim exact pure-scan cost, or add it to sweep time. Allocation order can bias deterministic
sampling, low counts are noisy, and clock resolution/observer overhead affect small values.

Outside the charged interval the binding emits:

```text
MANTLE_BENCH_PY_GC_SWEEP context=resume kind=automatic id=... path=... count=... pools=... arenas=... blocks=... free_blocks=... live_blocks=... dead_blocks=... empty_arenas=... empty_blocks=... released_arenas=... allocator_free_calls=... accounting_calls=... scan_destroy_ms=... partition_release_ms=... sample_cadence=256
MANTLE_BENCH_PY_GC_DTOR_SAMPLE context=resume kind=automatic id=... path=... count=... category=list dead=... samples=... sampled_subset_ms=... sample_cadence=256 allocator_samples=... charge_subset_ms=... libc_free_subset_ms=... charge_sample_max_ms=... libc_free_sample_max_ms=...
```

The second marker occurs for each of list/dict/string/tuple/host_handle/other, including zero
counts. Match the same outer-resume context/kind/id/path and occurrence as the existing
markers. This remains a diagnostic, not a performance improvement or per-owner CPU ledger.

The initialized ABI regression now checks exact block/category/freed reconciliation, five
pool visits, callback nesting, ceil(N/256) sample counts, exactly
15+2*destructor_sample_count+3*allocator_sample_count clock calls,
sample-time containment, real backing-free/accounting activity, and reset cleanup. Its
bounded garbage crosses a sampling boundary in all four builtin container categories.
Root must compile/run it, verify Shipping exclusion, then repeat the unchanged full50 with
the existing flag. Compare total pauses against the prior four-phase instrumentation;
per-block counter branches and sparse clock reads are additional measured observer overhead.

## Nested selected-free timing

Only while an already-selected destructor runs, each non-null MantlePocketPy_Free takes
three additional clock readings: immediately before its unchanged MantlePocketPy_Charge,
immediately after that charge, and immediately after the unchanged libc free. The charge
interval includes VM accounting and the original-owner callback; it does not isolate the
owner-map lookup from the rest of that callback. The second interval includes libc free
and boundary overhead. Header ownership, synchronous callback order, pointer-free order,
and every charge are unchanged. Unsampled objects and arena-release frees take no extra
clock readings. No malloc/realloc or allocator backend changes are part of this patch.

Fixed per-category POD records allocator_samples, both interval sums, and individual-free
maxima. These are nested subsets of sampled_subset_ms, which itself is a subset of scan
time. Maxima may belong to different frees, so do not add them as if they describe one
allocation. The three clock reads increase measured destructor/GC time; these observations
cannot be multiplied to claim a whole-collection accounting or libc cost. The sample scope
is -1 outside selected destructors, cleared before the sweep observer, and discarded before
VM close/reset through the existing active-record cleanup.

The initialized observer test additionally checks nested interval containment, actual
list/dict backing frees, two free samples per selected builtin dictionary, no backing frees
for builtin string/tuple objects, sample-clock counts, and scope cleanup. It does not claim
performance improvement, fair CPU attribution, or a preferred allocator backend.

## Binding owner-lookup cache (#158)

The unchanged full50 diagnostic at integration 73b2136, raw session
`b0dbf1eedae44e9ca72835603eef35e5`, still faulted before measurement. The two late
automatic collections took 11.769399 and 18.013000 ms. Within the existing selected
destructors, the recorded intervals were:

| Collection | Category | Selected frees | Charge subset ms | libc-free subset ms |
| --- | --- | ---: | ---: | ---: |
| 81,946 dead objects | list | 143 | 0.011396 | 0.007801 |
| 81,946 dead objects | dict | 298 | 0.026386 | 0.011500 |
| 160,175 dead objects | list | 279 | 0.021517 | 0.008184 |
| 160,175 dead objects | dict | 580 | 0.033900 | 0.014998 |

These observed charge subsets exceed the corresponding observed libc subsets. They do
not identify all charge time as map lookup, cannot be scaled into collection totals,
and include clock overhead/resolution and possible scheduling noise. They justify a
bounded experiment removing repeated lookup work, not a promised speedup or allocator
backend switch.

`Private/Python/MantlePocketPyMemoryAccounting.h` now owns the binding's accounting map
and a single last-owner entry pointer. Its complete mutation surface is Charge, Remove,
and Reset. Charge invalidates the pointer BEFORE every cache miss's FindOrAdd (whether
lookup finds an existing owner or insertion relocates entries), then caches only the
returned current entry. Remove invalidates before removing any key, including a different
or missing owner. Reset invalidates before resetting the map. Get performs only a const
lookup and exposes a value, never a reference. Copy/move are deleted so the map and cached
pointer cannot be copied apart. No mutable map is exposed to production callers.

The VM's sole C allocator callback still calls ChargeInstance for every original-header
owner/delta; ChargeInstance forwards each call immediately. Only its map lookup changes.
The addition, saturating subtraction, Remove/Reset, and observable late-free zero totals
are unchanged. The subsequent #153 host-retirement patch avoids allocating an absent
zero-valued owner entry on negative misses; numeric-ID reuse semantics remain unchanged.
All existing VM total accounting, quota flags, C callback ordering, CPU
charging, collection thresholds and allocator backend remain unchanged. The runtime is
still single-threaded; no new cross-thread use or aggregation is supported.

The production helper regression `OwnerAccountingCacheSurvivesMapMutation` uses real TMap
insertions until an earlier entry actually relocates, then checks immediate totals against
an uncached reference over owner switches, insertions, same/different/missing owner removal,
reset, and late negative charges. It also verifies invalidation and disallows stale-pointer
reuse after removal/reset. Root must build/run it and the initialized Python suite, then
measure the unchanged full50 before making a performance claim. Shared-GC CPU ownership
and nonincremental latency remain open even if repeated lookups become cheaper.
