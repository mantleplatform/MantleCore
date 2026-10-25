Six patches, each marked "MANTLE PATCH" in the file they touch. See ../VERSION for the full
explanation of each, and for why generator send (considered during the original lab
port) was rejected.

1. upstream/pocketpy.h: forward declarations for the three allocator hook functions
   PK_MALLOC/PK_REALLOC/PK_FREE expand to (MantlePocketPy_Malloc/Realloc/Free, declared in
   Public/MantlePocketPyHooks.h). Ported from ScriptLab's own patch
   (D:\UEProjects\ScriptTesting\Plugins\ScriptLab\Source\ThirdParty\PocketPyLib\patches\README.txt),
   renamed Lab* -> Mantle*.

2. upstream/pocketpy.c, VM__run_top_frame (the bytecode dispatch loop's __NEXT_STEP label):
   a deterministic per-opcode instruction-budget interrupt. NOT present in the lab's own
   vendoring -- added after the hosted integration build showed a runaway Python script's
   `while True: total = total + 1` loop freeze the whole game thread on frame 0. The
   py_sys_settrace-based budget MantlePocketPy.cpp used before this patch relied on
   TRACE_EVENT_LINE, which only fires when the *source line* changes; that loop's bytecode never
   crossed a line boundary the tracer noticed, so the budget never fired. The patch increments
   MantlePocketPy_OpCount[vm] every opcode and polls a host-installed function pointer,
   MantlePocketPy_Interrupt, once every MANTLE_POCKETPY_INTERRUPT_PERIOD opcodes (both declared
   in Public/MantlePocketPyHooks.h); on a false return it raises a RuntimeError and
   `goto __ERROR` right there, in the same opcode, the same way the existing PK_ENABLE_WATCHDOG
   block a few lines below it already does for a wall-clock timeout.

3. #123 initialized-binding sandbox gate: upstream/pocketpy.h's py_Callbacks adds an optional
   per-VM import_allowed(path) callback, initialized to NULL by VM__ctor. py_import consults
   it before relative resolution or cached/native/file lookup. Denial returns "not found" (0),
   preserving ordinary ImportError from import statements and AttributeError from missing
   module attributes. MantlePocketPy.cpp installs the policy only after trusted VM bootstrap.
   The same boundary patch validates object.__new__'s type argument before userdata access;
   Entity.__new__(None) can otherwise reach that unchecked upstream cast. No allocator,
   instruction, watchdog, or memory limit changes.

4. #123 native receiver ownership: supplied-class constructors such as Random.__new__,
   Enum.__new__, dict.__new__, and BaseException.__new__ previously accepted unrelated
   native classes; unbound native methods could then reinterpret incompatible userdata.
   The dispatch boundary now validates receiver ownership before invoking native C code:
   - py_bindmethod, py_bindmagic, and py_bindproperty tag nativefunc TValue.extra with the
     declaring type (positive for an instance, negative for a __new__ class/subclass).
     py_newnativefunc initializes the previously unused nativefunc extra to zero. No TValue
     layout change; the other types' existing extra meanings are untouched.
   - py_call, VM__vectorcall, and the four optimized item/contains bytecode paths preserve
     and check this metadata. Bound methods and copied aliases preserve the full TValue.
   - py_bind records registered class ownership in Function.clazz; both signature-native
     dispatch paths check it after argument binding. Its weak reference is safe here:
     ManagedHeap__mark roots every vm->types[i].ti->self for the VM lifetime, independent of
     module/class dictionaries or callable aliases. This path only handles cfunc functions.
   - Class py_bind sites are array2d/chunked_array2d __new__, Random.choices, list.sort, and
     disabled ComputeThread.submit_call; all declare cls/self explicitly. Module functions
     and py_bindstaticmethod retain zero (no implicit receiver). Python static/class methods
     keep their existing dispatch. Internal raw C API py_callcfunc remains trusted.
   - Instance checks allow subclasses. PocketPy has a single base/base_ti; derived native
     classes inherit is_python=false, so object.__new__ cannot allocate an incompatible
     native subclass. Cross-native constructors reject before allocating. Ordinary numeric
     argument coercion remains in the existing native implementations.
   Mantle additionally checks a handle's pointer against the existing host-minted cache
   before reading its userdata, using a set bounded by that cache's VM lifetime. Tests use
   the initialized sandbox, valid native controls, wrong receiver/constructor cases,
   copied properties, signature aliases after GC, and continued execution after rejection.
   No raw-memory operation probes, client allowlist, budgets, or memory cap changes.

5. #123 exception-frame GC initialization: py_BaseException__stpush emplaces uninitialized
   BaseExceptionFrame storage but previously only assigned locals/globals while the debugger
   was active. ManagedHeap__mark visits both fields unconditionally. Allocator reuse during
   the full Mantle automation sequence exposed invalid TValue pointers at that mark step,
   although the focused suite passed. Initialize both fields to nil immediately after emplace,
   before optional debugger capture (which can itself allocate). Retained nested-exception
   tests exercise ordinary allocation/release and repeated GC in the initialized binding.
   The mark traversal and existing GC checks remain enabled; no memory or budget changes.

6. #156 collectible host-script modules: py_newmodule still registers native/imported modules
   permanently and rejects duplicate paths. New C-only py_newmodule_unregistered returns an
   ordinary module without entering that registry; the Mantle instance roots it explicitly.
   This prevents same-ID StopScript/StartScript from aborting, without accumulating unique
   import names or clearing globals referenced by surviving catalog modules/functions.
   - ModuleInfo contains an embedded TValue cell for each unregistered module. Its self
     pointer points at that cell; registered modules retain their original registry cell.
     The embedded self value is not a GC root and does not keep an unreachable cycle alive.
   - Frame__ctor canonicalizes both module and module-valued globals to ModuleInfo.self.
     Functions and script-defined types inherit frame->module, so they no longer retain a
     pointer into a deleted Mantle FInstance. Frame globals backed by dict/locals retain
     their existing upstream handling. The heap is non-moving.
   - Mark frames' module, functions' module (when present), and TypeInfo.module. Existing
     globals/dictionary/closure/generator marking is unchanged. These explicit edges keep
     each stable cell's owner alive while a frame/function/type can access it.
   - Upstream's type registry remains VM-rooted: a class-defining script's module therefore
     remains alive with its types until VM reset. This patch does not introduce type-index
     reuse or claim to reclaim retired classes. Unreferenced classless modules and their
     ordinary globals/function cycles can be collected; initialized churn tests observe
     their entity wrappers being finalized after each stop and GC.
   - Catalog require still returns actual cached modules or explicit exports per side;
     old references retain their own globals while a reload creates a fresh instance.
     Sandbox import policy is unchanged and private module paths remain inaccessible.
   Mantle native callback wrappers separately capture a monotonic instance generation;
   IsAlive and Python re-export reject stopped/replaced/reset owners before reading an old
   Python value. Both generation and root-ID allocators survive same-VM RestartAll.

Bounded module review for this gate: sys has platform/version/argv and recursion controls
(setrecursionlimit removed), not sys.modules; inspect exposes signature/type/generator
predicates, not frames or globals; function/generator types expose no code/globals/frame
objects; gc exposes no heap-object enumeration and its mutation/debug controls are removed.
The allowed embedded libraries inspected do not import denied native modules. This review
and the receiver regressions are targeted boundaries, not a comprehensive upstream audit.

7. #153 fallible allocation foundation (not a hard cap): c11_vector__try_reserve
   delegates to the shared plain-C MantlePocketPy_TryVectorReserve primitive. It
   validates signed inputs, checks byte multiplication, stores realloc's result
   separately and commits data/capacity only after success. Length is untouched.
   - The old void reserve first clamps to four, including negative/zero requests,
     and retains its abort-on-failure behavior. Valid growth and no-op capacities
     are unchanged. The adapter still calls PK_REALLOC, retaining its original
     header ownership/accounting; no allocator limit or GC policy is changed.
   - Checked add/multiply/count helpers leave output unchanged on failure. They
     are also a foundation for later allocator-header and container conversions;
     those paths are not changed here. Earlier signed nextcap/extend arithmetic,
     partial constructors and exception/host unwind remain unresolved.
   - The reserve callback/context is explicit per call, not a mutable allocator
     hook. Production uses PK_REALLOC. Automation tests use the same C primitive
     with two fixed 64-byte buffers to force relocation, repeated denial and a
     successful retry, while checking old data/capacity and a sibling allocation.
     Synthetic large sizes are rejected before obtaining any backing memory.
     No test hook or failure switch is added to Shipping or exposed to Python.
   All callers must adopt and correctly propagate failure before the production
   allocator may deny a request. #153 remains a release blocker after this slice.

8. #159 / #153 checked growth and self-extension: list_extend formerly captured its
   source array pointer before reserve, so list.extend(self) could read the freed
   old allocation after realloc moved it. Its self-source branch now records the
   original logical span and derives the source from the replacement allocation.
   - Shared C append planning checks length addition, byte offsets, copy sizes and
     next capacity before mutation. Growth remains doubling below1024/25percent
     thereafter; reserve minimum4, legacy size<=0 no-op and fatal failure remain.
     External and self-slice helpers commit length after copying; failed growth
     leaves data, capacity, length and elements unchanged. No temporary Python
     list, extra backing allocation or change to PK_REALLOC ownership is needed.
   - The self span ends no later than the old logical length, where appended data
     begins; the rebased ranges cannot overlap. A zero-count empty self span does
     not form or dereference a data pointer. Ordinary raw sources must remain
     valid through growth, just as before; arbitrary raw C aliasing is not solved.
   - Twelve old raw extend call expressions were reviewed: list add/multiply/copy
     use new destinations; list_extend is the explicit self-source case; serializer
     and pickle writers copy local scalars, source strings/objects or code buffers;
     deserialization copies its distinct input storage; sbuf copies literals,
     source text, Python strings or detached formatted bodies (submit then reinit).
     Public/native callers still own their source-lifetime contract. Earlier
     source-length multiplication/narrowing in those callers is not repaired here.
   - Bounded tests force moves between two64-byte buffers and poison the old span,
     including full/interior self slices, failed growth/retry, no-growth copies,
     external append and sibling preservation. Pure planning tests cover INT_MAX
     without backing allocations. The initialized Python regression checks real
     list self-extension, object/function identity, cyclic/empty cases and GC.
   Fallible emplace/copy/push/insert propagation, bulk string/list overflow,
   constructor cleanup, nonallocating fault delivery and quota denial remain #153
   work. Checked helper failures are not yet recoverable Python memory faults.

#158 additive optional GC diagnostics: upstream mark/sweep exposes four subphase boundaries
through a separate native observer. Hooks, counters and logging are excluded from Shipping;
the existing benchmark flag installs them otherwise. No collection policy or budget change.
See GC-DIAGNOSTICS.md for the phase/whole-total relationship, counts, lifetime and ABI test.
The follow-up adds fixed small-sweep POD counters, pool scan/destruction vs partition/release
timings, and category-local sparse destructor samples. Samples are observed subsets, never
extrapolated exact costs. Allocator/accounting hooks only count existing calls; no policy,
allocation ownership or budgets change. The same document defines overhead and limits.
Nested selected-free timing further splits actual sampled destructor calls into existing
charge and libc-free intervals with per-free maxima. These are subsets, not estimates of
whole-collection costs; allocator backend, callback ordering and every charge are unchanged.

9. #160 / #153 bulk sequence representability: string add/multiply and list
   multiply formerly formed unchecked signed sums/products or narrowed py_i64
   repeat counts before validation. Strings could allocate for a truncated size
   then copy using the original wide count; empty repeats could loop needlessly.
   - Pure checked helpers validate sums and repeats before forming products.
     Empty sources/count<=0 produce zero without negating INT64_MIN. Positive
     counts are compared to maximum/source_length while still int64_t.
   - String operators derive their payload maximum from INT_MAX minus actual
     sizeof(PyObject), sizeof(c11_string) and the NUL byte. This bounds both int
     additions on the py_newstrn -> ManagedHeap__gcnew path and keeps full object
     size within the positive-int input domain of the eight-bit size codec.
     General py_newstrn/heap constructor contracts are deliberately unchanged.
   - List preflight walks only successive growth boundaries via TryPlanAppend,
     validating the original incremental schedule without allocating. It catches
     next-capacity overflow even when final logical length itself fits int.
     Valid results retain their original append/reserve sequence. The fixed list
     object header is separate from vector backing bytes; element byte checks
     use sizeof(py_TValue), not an assumed per-element header adjustment.
   - Rejection uses the existing bool-returning ValueError seam BEFORE result
     allocation/copy loops. Error formatting and exception construction still
     allocate; there is no nonallocating memory-fault or hard-quota claim. Type
     mismatch/NotImplemented and mirrored sequence multiplication stay intact.
   - Pure tests use synthetic integer/header boundaries and compare repeat plans
     with modest step-by-step append planning. Initialized tests cover modest
     contents/identity/UTF-8, GC, empty/nonpositive repeats, mirrored dispatch and
     huge COUNTS guaranteed to reject, never huge valid allocation requests.
   Representable results may still exhaust memory. Allocator denial, header and
   ownership accounting, all other bulk/constructor paths and safe fault unwind
   remain unresolved #153 work. No CPU/memory budget or GC policy is changed.

10. #153 typed vector alias/storage foundation: push formerly read its element
    after realloc, and insert read after realloc AND memmove. Native callers
    passing a py_list_getitem reference could use freed storage or a shifted
    element, even with no growth. Shared C11 typed macros now capture vector,
    index and element exactly once before mutation, then call checked helpers.
    - Actual vendor push/insert macros use the same tested materialization,
      preserving type alignment, capacity growth and fatal legacy failure.
      The allocator must not collect/reenter; a TValue local is not a GC root.
      Source values are evaluated before allocation. Ownership-transferring
      expressions (notably compiler Ctx__s_popx) will need caller cleanup when
      recoverable errors are introduced; this patch still aborts on failure.
    - Insert checks index/tail arithmetic before reserve, then shifts/copies and
      commits length. Emplace/copy wrappers consume new fallible C primitives.
      Failed operations preserve outputs and existing allocation contents.
      Emplace success still publishes uninitialized storage; copy success owns
      an independent block with source capacity/minimum-four semantics.
    - Bounded tests use the same typed macros with moving/poisoned tiny buffers,
      count operand evaluation, and verify denial/retry, shift-only insertion,
      synthetic overflow, emplace output and copy ownership. An initialized
      binding test passes actual native list interior references through public
      append/insert and verifies object identity after GC; all list slots are
      initialized before allocations can collect.
    This is not quota denial or nonallocating error propagation. Multi-allocation
    transactions (including dict.copy/rehash), constructor cleanup, compiler,
    collector and nested VM/host fatal unwinding remain separate #153 work.

11. #153 private root retirement: generic dict deletion hashed through mutable
    int magic slots, overwrote retval, and allocated a compaction mapping. The
    binding now uses an explicit private-table pair from empty table creation:
    canonical exact-int insertion and callback-free, VM-independent discard.
    - This is NOT general dict semantics. Tables contain exact integer keys with
      canonical cached hashes and are never exposed to Python. Both operations
      bypass mutable int.__hash__/__eq__; general dict methods stay unchanged.
    - Set retains ordinary vector/table growth and fatal allocation behavior.
      Discard repairs the probe cluster and, at the unchanged deletion threshold,
      stably compacts entries and rebuilds the existing index array in place.
      Capacities and short/long index width remain unchanged. No scratch buffer,
      allocator/free, GC, VM selection, exception/retval mutation or dispatch is
      involved; direct field access and immediate TValue constructors only.
    - FCallable destruction consumes VM-independent Unroot without EnsureCurrent.
      Dropping roots defers object destruction to tracing GC. This does not make
      Close/RestartAll, reporting, host subscription retirement, or allocator
      accounting callbacks allocation-free.
    - Non-Shipping initialized tests exercise actual roots, pending exceptions,
      result preservation, mutated/restored int magic slots, repeated bounded
      churn, survivor GC identity/order, wait/deferred/module retirement, sibling
      liveness and cross-VM callable destruction. A bounded synthetic table
      exercises wraparound collisions and both index widths using the same C
      helpers, without allocating a huge long-index table. Scoped POD counters
      observe malloc/realloc/free entry attempts; Build.cs explicitly excludes
      diagnostics and counter branches from Shipping. No allocator is replaced
      or made denying. No hard-cap/fatal-propagation claim is made.

12. #158 dictionary storage co-allocation: one original-owner allocation for
    entries followed by indices, with checked dict-specific growth/copy and
    overlap-safe tail movement. Dict/PyObject layouts, index representations,
    load thresholds and entry-capacity progression remain unchanged. Allocation
    headers and all actual charges are retained; one physical allocation/header
    disappears. Local bounded moving/denial tests and initialized ownership,
    semantic and GC regressions are documented in DICT-STORAGE.md. No collector
    policy, attribution, limits, workload or allocator backend change. Performance
    awaits measurement; legacy callers still terminate on allocation failure.

    #158 diagnostic comparison: the binding now accepts
    -MantleScriptBenchGcTotalsOnly under the existing validated benchmark flag.
    It keeps the whole-GC observer/two clocks, installs no phase/sweep observer or
    clock, labels mode in whole receipts, and omits unavailable detail records.
    Default detail and all collection/budget policies remain unchanged. The mode
    is absent from Shipping. See GC-DIAGNOSTICS.md for comparison limitations.

13. #153 quota reservation foundation (production denial still disabled): each
    real script generation receives stable POD quota state at a healthy host
    boundary before its module allocation. Allocation headers retain that exact
    cell through instance retirement and numeric-ID reuse; a late free therefore
    debits the original generation without allocator-time map lookup or growth.
    Drained retired cells are reclaimed only at a later healthy boundary.
    - Checked header addition rejects size_t wrap. TryReserve atomically checks
      committed+reserved bytes against the unchanged 16 MiB instance and 128 MiB
      VM ceilings before mutating either counter. Commit transfers reservation to
      committed bytes; rollback releases it. All three are POD-only and invoke no
      Python, host callback, allocator, logger or dynamic container.
    - The admitted malloc/realloc/free surface takes explicit bounded allocation
      callbacks. Denial precedes the callback, libc-equivalent failure rolls back,
      and realloc reads the original header so failure preserves its pointer,
      bytes, owner and charge. Legacy numeric-owner blocks are rejected rather
      than silently changing their callback/debit route.
    - Deterministic automation uses two fixed 512-byte buffers and synthetic
      reservations (never giant backing allocations) to cover both ceilings,
      repeated denial, malloc/realloc failure, retry, payload/owner preservation,
      quota balance, same-ID generation replacement and exact late free. It also
      asserts neither the legacy nor active-cell map grows during admission.
    PK_MALLOC/PK_REALLOC intentionally remain on the non-denying wrappers until
    the complete upstream failure graph and allocation-independent terminal path
    are converted. No creator API or runtime terminal capability is exposed;
    #153 remains release-blocking after this foundation.

14. #153 allocation-fatal POD channel (production denial still disabled): the
    fallible admitted allocator records the first size-overflow, quota-denial or
    backing-allocation failure in fixed per-VM storage. The record copies reason,
    VM, instance and generation without allocation, logging, Python API, host
    callback, longjmp or retained cell pointer. Later failures cannot overwrite
    it; take/reset are fixed-field operations.
    - The host consumes only after the outer RunBudgeted call has returned from
      PocketPy and nested native/C++ work. Existing host-requested fault ordering
      is preserved. A record can retire only a currently live quota cell whose
      VM/id/generation all match; retired/stale records and same-ID replacements
      are consumed without affecting a sibling or replacement.
    - Init/Close reset the slot channel. Test-only C++ injection is not bound to
      Python and is absent from Shipping. Bounded tests cover first-record wins,
      exact ownership/reason, map-storage stability, sibling continuation,
      retired/stale isolation, same-ID replacement and VM reset.
    Broad upstream callers still use the non-denying allocator and aborting or
    unchecked allocation contracts remain. The channel does not enable a hard
    cap, creator-visible MemoryError, or complete failure propagation; #153
    remains release-blocking.

15. #153 admitted vector-reserve boundary (no upstream caller converted): a
    shared explicit-result helper now joins checked element-byte arithmetic to
    the admitted malloc/realloc path. Empty vectors receive the supplied stable
    generation cell; growth retains the allocation header's original owner.
    Quota/backing failure leaves pointer, capacity, contents and charges intact,
    while the admitted allocator records the exact first fatal. No-op/invalid
    input neither allocates nor records a terminal fault; legacy numeric-owner
    blocks are rejected rather than silently changing their debit route.
    - Deterministic automation uses the unchanged 16 MiB instance and 128 MiB VM
      ceilings, tiny real backing blocks and an exact synthetic reservation. It
      covers repeated pre-libc denial, first-fatal identity, successful retry,
      original bytes/charge, sibling isolation, cleanup and no accounting-map
      growth. No large backing allocation or global allocator override is used.
    - Frozen-inventory disposition: c11_vector__try_reserve at the common resize
      family (around upstream/pocketpy.c:12746) remains on c11_vector__reallocate
      -> PK_REALLOC. Its reserve/emplace/extend/copy wrappers still abort, as do
      list.extend(self) and the Dict try-family's legacy wrappers/callers around
      32560-32630/33415. None is switched to this admitted helper.
    A native bool caller cannot simply return false after allocation denial:
    VM__run_top_frame's error path requires an existing Python exception, whose
    construction may allocate, and debug py_callcfunc explicitly aborts when
    false has no exception. A nonallocating VM terminal-unwind result must be
    added and propagated before one of these families can safely become the
    first production caller. #153 remains release-blocking.

16. #153 nonallocating terminal unwind (production denial still disabled):
    FrameResult now has an internal RES_TERMINAL distinct from RES_ERROR. A
    fixed-array POD probe selects it only when the allocation-fatal channel's
    copied VM/instance/generation exactly matches the active current quota cell
    and no ordinary Python exception is pending. A sibling, retired generation,
    same-ID replacement or binding/setup path with no current cell cannot gain
    terminal authority from another owner's queued record; the outer host
    checkpoint still takes that record and retires only its exact owner.
    VM__run_top_frame clears no
    traceback, constructs no exception, and pops frames only through the current
    invocation's base before returning the result across native/C++ frames.
    - Touched error/result paths are explicit: opcode vectorcall dispatch;
      prepare_py_call failures; plain and declaration-bound native calls;
      recursive type __new__/__init__ and __call__ lookup; optimized-magic/helper fallthrough through
      the central __ERROR label; generator next; pk_exec/pk_execdyn; public
      py_vectorcall/py_call; and debug py_callcfunc's false-without-exception
      contract. Ordinary false+exception remains RES_ERROR and follows unchanged
      traceback/handler behavior. Terminal retval is nil and no exception is set.
    - Native wrappers also observe a fatal recorded by nested work even when the
      outer native returns true. This is required for task.spawn: its nested
      RunBudgeted call returns through C++, then the outer VM unwinds and only
      the true outer host checkpoint consumes/reports the exact generation.
      ReportPendingException is a no-op when no Python exception exists. The
      checkpoint's pre-existing host-pending-fault-before-allocation-fatal order
      is unchanged.
    - Non-Shipping deterministic tests install private native functions only in
      test instance modules. They cover direct native unwind, native->py_call
      nested unwind, task.spawn nested RunBudgeted propagation, nil retval/no
      exception distinction, exact owner retirement, same-ID replacement,
      sibling continuation, unrelated binding/native dispatch while a sibling
      fatal is pending, and unchanged ordinary ValueError state/continuation.
    Remaining conversion blocker: all production allocation callers still use
    legacy aborting/unchecked wrappers. The first caller family must replace its
    shared aborting wrapper with the admitted explicit-result API and prove every
    partially constructed value/ownership transfer is rolled back before it may
    record a fatal and return into this terminal path. This patch itself denies
    no production allocation and exposes no creator API; #153 remains incomplete.

17. #153 fallible non-recursive native representation family (hard cap still
    incomplete): `float/object/type/str/bytes/function/module.__repr__` and
    `Enum.__str__` now use admitted, explicit-result sbuf construction, atomic
    char/C-string/float/pointer/quoted writes, and submit. Each denied reserve
    preserves the old pointer, length, capacity and bytes; submit publishes no
    partial `c11_string`. False creates no Python exception and selects the
    exact-generation terminal path, so creator catch-and-continue cannot swallow
    denial. Fixed 512-byte callbacks cover injected move/failure/retry, formatted
    output and sibling preservation; initialized tiny-quota bytes repr covers
    the real terminal path and ordinary semantics cover every converted leaf.
    Legacy sbuf callers, direct c11_string allocation, and managed Python string
    construction remain unconverted. Production PK allocator wrappers are not
    globally denying. See STRING-BUFFER-FALLIBLE.md for the caller ledger.
    This follow-up adds fully preflighted `str.zfill/ljust/rjust`: wide
    widths are validated before narrowing or loops, the entire sign/source/pad
    result reserves once, and logical length commits only after all bytes are
    copied. Fixed-array denial/retry and initialized exact-generation terminal/
    sibling tests remain modest. Nested-dispatch mod/format stay explicitly
    unconverted; the later managed-string follow-up below converts join.

18. #153 managed-heap admitted tracking prerequisite (production constructors
    still legacy): `ManagedHeap__try_gcnew` now has a checked, explicit-result
    path for non-dict managed objects. Small-object backing arenas use a private
    intrusive list; large objects co-allocate a max-aligned intrusive prefix.
    Neither path can require an aborting vector growth after quota admission.
    Publication of outputs, lists and heap counters occurs only after backing
    allocation and initialization succeed. Sweep/destruction include both the
    unchanged legacy vectors and the admitted lists, and `PK_FREE` consumes the
    retained allocation header's exact original generation even when a sibling
    is current. Existing arena retention and heap usage reporting include the
    admitted set. Checked signed/int/size arithmetic rejects negative and
    overflowing shapes before storage is requested.
    - A non-Shipping initialized diagnostic uses modest allocations and injected
      denial to cover invalid/overflow shapes, repeated quota denial before the
      backing callback, repeated backing failure with reservation rollback,
      arena and large success, zero legacy-vector growth, max-aligned large
      payload, exact header ownership and sibling-current cleanup.
    - At this patch stage, `slots == -1` was rejected because its NameDict ctor
      performed a second legacy allocation. Item 21 adds the failure-atomic
      two-allocation substrate without switching legacy production callers.
      No `py_newstrn`, `py_newstrv` or other legacy constructor calls the new
      path, so caller terminal propagation remains conversion work.
    See MANAGED-HEAP-FALLIBLE.md. This is not the completed hard cap; global
    PK_MALLOC remains non-denying and #153 remains release-blocking.

19. #153 first admitted managed-string production slice: additive
    `py_trynewstrn`/`py_trynewstrv` preserve outputs on failure, enforce the
    inline-15/managed-16 boundary, check complete string payload arithmetic, and
    publish only after admitted construction/copy succeeds. The converted set is
    limited to int repr, chr, string add/multiply/case/strip/integer-getitem, bytes
    decode, and final publication for the already-fallible sbuf repr/padding
    leaves. Sbuf denial always releases detached storage before terminal return.
    - Deterministic diagnostics cover embedded/trailing NUL, both representation
      boundaries, unchanged output/retval/SP, exact fatal identity, quota
      rollback and submitted-buffer cleanup. An initialized nested native test
      denies a modest dedicated bytes-decode result, proves the exception handler
      cannot continue, retires only the exact owner and preserves a sibling.
    - Slice-getitem, aggregate split/splitlines/replace/format, iterators, name/cache/
      compiler/module/host/error/traceback and serializer paths remain legacy;
      so do `py_newstrn/v`, direct c11_string creation and production
      slots==-1 callers (the additive substrates are updated in item 21).
    See MANAGED-STRING-FALLIBLE.md. Global allocation remains soft for every
    unconverted caller and #153 remains release-blocking.
    This follow-up converts only `str.join`. The function now explicitly owns
    and restores its iterator stack root, uses admitted fallible sbuf ctor/write/
    try-publication, preserves ordinary iterator/type exceptions, and frees
    attached or detached storage on every denial. Bounded root diagnostics cover
    embedded NULs, long separator/items, ctor/growth/publication denial,
    unchanged retval/stack state, exact generation terminal and sibling health.
    The next bounded follow-up converts only detached `str.replace` and string
    slice temporaries. Their helpers preflight exact output bytes, allocate once
    through the admitted path, publish output pointers last, and their public
    callers always free the temporary after fallible managed publication.
    Fixed-array/injected backing denial and modest managed-publication denial
    cover unchanged output/retval, reservation rollback, embedded NULs, UTF-8
    forward/reverse stepped slices, exact terminal ownership and sibling health.
    A bounded linear span walker treats malformed/truncated UTF-8 bytes as
    individual units, so bytes.decode output cannot cause an out-of-span read or
    an under-filled result; fixed malformed and 384-byte multibyte cases cover
    both directions without timing assertions.
    `c11_string__new3` and all six import/cache callers remain legacy because
    compiler/bootstrap/cache paths are outside this slice.
    The split follow-up converts only the current no-maxsplit `str.split` and
    `str.splitlines` contracts plus exact span planning and private list/string
    publication. Real helper/list-backing/second-element denial plus a clearly
    identified caller-boundary list-object failure seam leave retval and roots
    unchanged, select the exact-owner terminal, and cover deferred managed
    element reclamation. Embedded NULs, malformed/truncated
    splitlines bytes, existing whitespace/newline/keepends behavior and the
    ordinary argc>2 TypeError are covered with bounded diagnostics. The legacy
    value-returning `c11_sv__split` remains solely for the excluded package
    component parser; import/module ownership is not broadened.

20. #153 admitted bytes-family slice: additive `py_trynewbytesn` coallocates the
    bytes header and payload through the admitted managed heap and preserves
    output TValue/data pointers on denial. Existing `bytes()` list/tuple
    construction, `str.encode`, bytes concat, bytes slicing and the already-
    admitted decode path now fill private/local results and publish retval last.
    Constructor elements are prevalidated, concat size is checked, and slicing
    exact-sizes directly instead of growing an aborting temporary vector.
    - Bounded diagnostics inject a large-object backing failure after real
      reservation and separately impose real object/payload quota denial. They
      verify rollback, unchanged retval/stack/sibling, exact terminal identity,
      malformed-byte decode preservation, owner retirement and sibling health.
    - This runtime has no bytes repeat operator; none is added.
    The byte-literal follow-up converts only interpreter `OP_BUILD_BYTES`. It
    fills a local admitted bytes value and pushes only after completion; denial
    leaves the operand stack unpublished and enters the exact terminal unwind.
    A post-compilation quota clamp exercises the real opcode and verifies safe
    SP/retval/sibling state, unchanged accounting, owner-only retirement, and
    ordinary embedded-NUL/malformed literal behavior. Compiler/token vectors
    and constant ownership are unchanged.
    The base64 follow-up converts only `b64encode`/`b64decode` result allocation
    to `py_trynewbytesn`. Each transform fills and resizes a local bytes TValue,
    then publishes retval last. Widened encode-capacity arithmetic avoids signed
    overflow before narrowing. Deterministic large-result quota denial exercises
    both real module calls and verifies SP/retval/accounting atomicity, exact-cell
    terminal propagation, owner-only retirement, and sibling health.
    Current inventory correction: LZ4 and the binary-IO byte-result path are
    absent from the active build; pickle bytes load/output and `stdc_read_bytes`
    are policy-blocked. The legacy `py_newbytes` definition and optional source
    still remain and are not hard-capped.
    See MANAGED-BYTES-FALLIBLE.md. Global allocator policy remains soft and
    #153 remains incomplete.

21. #153 admitted constructor substrate: additive
    `c11_string__try_new/new2/new3` now provide checked, output-preserving direct,
    sized, and custom-formatted string construction through admitted allocation
    and the existing fallible sbuf. Additive `py_trynewobject` publishes TValue
    and userdata together; `ManagedHeap__try_gcnew(slots == -1)` pre-admits the
    initial four-entry NameDict table and rolls it back if object/arena admission
    fails. No legacy wrapper is switched.
    - Root diagnostics cover small/large/embedded-NUL/formatted strings,
      injected backing failure, repeated quota denial, retry, unchanged
      output/retval/SP/accounting, repeated NameDict first/second-allocation
      denial, no partial heap publication, exact table owner, GC/sibling-current
      teardown, public dict-backed output preservation, and zero/negative/high
      type rejection before deterministic fresh/reused/large allocation shapes.
    - Direct legacy c11 callers remain SourceData filenames, compiler token and
      function names, ModuleInfo package/path, six import/cache filename forms,
      traceback names, copy, and one-character replace. Supported-reachable
      dict-backed callers remain generic instance, type/reload, and module/
      bootstrap/cache transactions; the OS path caller is compile-excluded and
      pickle deserialization is policy-blocked. Later NameDict rehash is legacy.
    - Current active-build inventory has no reachable LZ4 or binary-IO byte
      result path. Pickle/stdc are policy-blocked. Broad production PK allocation
      remains soft until all reachable callers are terminal-safe.
    See CONSTRUCTOR-SUBSTRATE-FALLIBLE.md. #153 remains incomplete.

22. #153 admitted SourceData/compiler-entry transaction: additive
    `SourceData__try_rcnew` allocates the refcounted owner and filename through
    admitted explicit-result storage, normalizes the C-string source through the
    fallible sbuf (BOM skip, CR removal, trailing-whitespace trim, terminal NUL),
    and pre-admits capacity for every line start before publishing the object.
    `_py_compile` consumes the result and returns false with the fixed exact-cell
    fatal and no Python exception when construction is denied.
    - Root diagnostics deny object, filename, initial source buffer, source
      growth, submit-NUL growth, and line-start storage twice each; inject backing
      failure into object and filename allocation; then retry. They verify
      unchanged outputs/retval/SP/accounting, exact header ownership, embedded
      NUL termination, normalized bytes, null-source bytecode metadata, ordinary
      `SyntaxError`, exact owner retirement, and sibling continuation.
    - SourceData preplans all line-start capacity, so lexer newline insertion no
      longer allocates. Item 23 converts the public path's token/indent vectors;
      Compiler/CodeObject vectors and their constructors remain legacy and
      soft-capped. The remaining
      `SourceData__rcnew` users (bytecode deserialization and native signature
      compilation) retain the aborting compatibility wrapper; `py_execo` enters
      deserialization rather than `_py_compile`.
    See SOURCE-DATA-FALLIBLE.md. Global PK allocation remains soft and #153
    remains incomplete.

23. #153 admitted lexer queue transaction: the public `_py_compile` path now
    selects fallible token and indentation vectors plus fallible string/bytes/
    f-string payload buffers. The transaction publishes token storage only
    after EOF; denial destroys every queued owned payload and both vector
    backings, preserves caller outputs, and returns the fixed exact-generation
    fatal without manufacturing a `SyntaxError`.
    - Legacy native-signature compilation keeps the existing aborting lexer
      mode. SourceData normalization and line-start behavior are unchanged.
      Compiler contexts, CodeObject/codegen vectors, raw parser constructors,
      bootstrap/modules/imports, traceback formatting and global allocator
      policy are not converted.
    - Non-Shipping root diagnostics deny the initial/growing token vector,
      initial/growing indent vector, and payload ctor/growth/submit allocation
      twice each, then retry through the same implementation. They check exact
      allocation ownership, output/retval/SP/accounting preservation, payload
      cleanup, ordinary indentation `SyntaxError` distinction, production-entry
      terminal retirement and sibling continuation.
    See LEXER-QUEUE-FALLIBLE.md. #153 remains incomplete.

24. #153 compiler/codegen vector transaction: public in-memory compilation now
    stages CodeObject names/root blocks, contexts, blocks, paired bytecode,
    constants, function declarations, and CodeObject/FuncDecl var/name/arg/
    kwarg vectors behind explicit-result admission. Failure latches the fixed
    exact-generation terminal and destroys the unpublished graph exactly once.
    - The admitted constant path uses CodeObject-local linear string dedup to
      avoid legacy raw small-map key ownership. Compiler-created managed strings
      and default tuples are tracked until publication; rollback immediately
      destroys them, returns small blocks/unlinks large objects, releases only
      transaction-created admitted arenas, and restores the GC counter. Tracking
      denial retains the newest untracked value for the same unwind. Native
      signatures and deserialization retain legacy constructors.
    - Parser AST/raw vectors are converted by item 25. Errors, modules/imports,
      serialization, host caches, and global allocator policy are unchanged.
      Root and initialized
      host diagnostics cover repeated vector and managed-value/tracking denial,
      later denial after managed constant/default creation, f-string expression
      and multi-case local-patch unwind, retry, ordinary SyntaxError, exact owner
      retirement, and sibling continuation.
    See COMPILER-CODEGEN-FALLIBLE.md. #153 remains incomplete.

25. #153 parser-owned storage transaction: the admitted public compiler path
    now covers every expression-node allocation, exact-size sequence child
    array, context expression-stack growth, positional/keyword call-vector
    growth, chained-comparison jump vector, and match-case patch vector.
    - Node and vector publication is failure-atomic. Child-consuming constructor
      failure, stack publication failure, call append failure, and temporary
      patch unwind each retain a single exact owner and destroy it exactly once.
      The existing compiler latch preserves the fixed exact-generation terminal
      and rolls back tokens, contexts, managed constants, and CodeObject output.
    - Root diagnostics deny all twelve object/array/vector initial-and-growth
      stages twice, retry, check output/retval/SP/accounting and ordinary parser
      SyntaxError distinction. A focused
      initialized automation denies a production parser node, retires only its
      exact owner, drains the fatal, and continues the same sibling generation.
    - Native-signature compilation remains legacy. Parser global-name maps,
      imports/package buffers and SyntaxError allocation are not converted;
      modules/bootstrap/cache publication, generic runtime constructors,
      PushValue, traceback/error formatting, serialization/deserialization and
      global allocator policy remain out of scope.
    See PARSER-STORAGE-FALLIBLE.md. #153 remains incomplete.

26. #153 admitted compiler smallmaps: the public in-memory compiler transaction
    now owns initial capacity and growth for `Ctx.global_names`, the paired
    `CodeObject` varname/name inverse maps, and `FuncDecl.kw_to_index`.
    - Compiler-local planning reserves vector and inverse-map storage before
      atomic entry/length publication. Denial retains the fixed exact-owner
      allocation terminal and the enclosing rollback restores accounting.
    - Root diagnostics deny all eight initial/growth stages twice, retry,
      validate persistent map ownership and vector/map agreement, account for
      the temporary context map, preserve output/retval/SP/sibling state, and
      distinguish ordinary SyntaxError. The initialized terminal diagnostic now
      denies the first inverse map before checking owner retirement and sibling
      continuation.
    - Generic `c11_smallmap`, native-signature compilation, debugger and pickle/
      CodeObject deserialization, imports/package buffers, context string dedup,
      modules/bootstrap/cache, and global allocator policy remain unchanged.
    See COMPILER-SMALLMAP-FALLIBLE.md. #153 remains incomplete.

27. #153 NameDict/unpublished-object substrate: additive failure-atomic
    NameDict reserve/rehash/set plus public `py_trysetdict` and
    `py_tryemplacedict` seams retain each table's exact allocation generation.
    A generic managed-object transaction can commit or discard one admitted,
    unpublished object with GC-list/counter/accounting and destructor safety;
    compiler managed-value rollback now shares that primitive.
    - A dedicated internal transaction pin survives any number of nested
      sweeps during an armed destructor and is cleared only by the outer
      discard. A custom destructor collects twice and verifies one destructor
      call, one block return, no duplicate unused index, and exact pool/arena,
      counter, SP/retval and accounting restoration.
    - Root diagnostics repeat initial and rehash denial twice, retry, preserve
      absent attributes/output/retval/SP/accounting, reject growth against an
      exact retired owner without charging a sibling, and distinguish ordinary
      `AttributeError`/nonallocating replacement from allocation terminal state.
    - No production compound constructor is converted. List/tuple/dict/slice/
      function/type constructors, modules/import/cache, host PushValue/roots/
      callbacks, errors/traceback, global denial and capabilities are unchanged.
    See NAMEDICT-SUBSTRATE-FALLIBLE.md. #153 remains incomplete.

28. #153 fallible function/closure publication: `OP_LOAD_FUNCTION` now owns an
    unpublished admitted function through `ManagedHeapObjectTxn`, snapshots
    lexical locals into a private admitted NameDict, inserts its recursive self
    binding before stack publication, and commits only after all steps succeed.
    Failure discards the function, closure and declaration reference exactly
    once and takes the existing exact-generation terminal path. Legacy public
    constructors and the ordinary special-locals RuntimeError are unchanged.
    `FuncDecl` stores an already-interned recursive name at construction (and
    reconstructs it on deserialize), so the opcode has a non-null precondition
    and does no name-table work after admission. Root diagnostics construct and
    arm a real Function around injected object/header/items/growth denial,
    then verify declaration/closure rollback, retry and sibling-current
    cleanup; initialized automation executes a recursive closure plus a
    first-use nested lambda. Type/class creation, modules/imports and global
    allocation policy remain out of scope. See FUNCTION-CLOSURE-FALLIBLE.md.

29. #153 container bytecode slice: additive admitted tuple/list/dict/slice
    constructors are used only by BUILD_TUPLE, BUILD_LIST, BUILD_DICT and
    BUILD_SLICE. Each opcode leaves its operand range and public result
    unpublished until all admitted allocation and infallible payload copies
    finish. Dict literals retain their unpublished object transaction and a
    temporary GC root through Python hash/equality; they reacquire stack and
    dict storage after callbacks and preflight backing growth/rehash before
    entry publication. Existing dict APIs and every other opcode remain legacy.
    Transactions identify their gc-counter credit by collection epoch, so a
    post-collection discard cannot debit a later allocation; no-GC rollback
    remains exact. Rehash retains the initial cached probe/hash and never
    replays stateful Python callbacks; publicly visible admitted list slots
    are cleared before their length is committed. Root diagnostics check exact
    restoration and cleanup across object/backing/growth/rehash denials. The
    initialized ContainerBytecodeFailureAtomic automation forces GC in a real
    failing dict literal, proves collision callback counts at rehash, and uses
    a diagnostics-only exact admitted fail-site marker to terminally deny each
    of the four production BUILD opcodes while checking exact owner retirement
    and sibling continuity. See
    CONTAINER-BYTECODE-FALLIBLE.md. #153 remains incomplete.

30. #153 compiler class BEGIN transaction: `OP_BEGIN_CLASS` preflights a
    replacement type table when needed and constructs a pinned admitted type
    object plus initial NameDict before one allocation-free publication step.
    Reload preflights current-owner NameDict backing, then swaps/reinitializes
    the existing object and index before body execution. After BEGIN, body,
    decorator, on-end and binding failures never roll back or tombstone the
    exposed class; hard-cap safety does not imply rollback of Python-visible
    side effects. Class-attribute and module final-binding growth now propagate
    the exact-generation terminal. Root diagnostics force type-table, initial
    NameDict, and fresh-pool-arena object denial (so arena reuse cannot hide
    the third stage), then verify exact rollback and repeat-safe fixture reuse.
    Initialized automation executes a real BEGIN terminal as well as
    post-exposure binding/on-end terminals, decorated retention,
    GC/dispatch/isinstance, reload identity/failure and sibling continuity.
    Native/bootstrap constructors, dynamic attribute APIs, imports,
    deserialization and global policy remain legacy.
    See CLASS-BEGIN-FALLIBLE.md. #153 remains incomplete.

31. #153 compiler-only fallible name interning: first-seen source identifiers
    and fixed grammar names `self`/`_` in the admitted public compiler are
    size-checked and quota-admitted before process-global table publication.
    Lookup/publication are locked, while all allocation, denial and losing-race
    cleanup happen outside the lock.
    - Successful buckets detach from the compiling temporary generation cell
      while retaining their VM charge, matching the table's process lifetime;
      finalization never dereferences a retired instance cell. Legacy
      `py_name`/`py_namev` callers remain unchanged.
    - The compiler latch returns the existing exact-generation terminal without
      publishing output. Constructor staging precedes CodeObject/context
      publication and stores both fixed names for context emission.
      `FuncDecl.recursive_name` is interned before declaration allocation,
      preserving named-function and lambda coherence.
    - Source class identifiers, normal-import bindings/aliases, from-import
      attributes and from-import bindings/aliases share the admitted compiler
      path. Root diagnostics cover those routes, overflow, empty-bucket retry,
      collision chains, named functions, nested lambdas, and a deterministic
      second-lookup race with exact losing-candidate cleanup. Output/chain/
      FuncDecl/SP/retval/accounting, exact name-intern terminal identity and
      sibling continuity are checked. The host terminal fixture uses a bounded
      deterministic scan and atomically arms denial with the locked final proof
      that its process-global name is absent, making repeat runs exercise the
      name-intern checkpoint without allocation or callbacks under that lock.
      Separate fresh diagnostic spellings force both fixed constructor slots
      through first-publication denial/retry without relying on bootstrap state.
      Runtime dynamic attributes, class/type
      construction, import path/module loading, serialization/deserialization
      and global quota policy remain unchanged.
    See COMPILER-NAME-INTERN-FALLIBLE.md. #153 remains incomplete.

32. #153 generator staging slice: generator-function variadics, exact-owner
    frames, generator/list publication, continuation restore/snapshot growth,
    and exception-frame growth now use narrow admitted transactions. Creation
    publishes no return value or host task until the frame, object, list and
    initial continuation are complete. Yield snapshot denial preserves the
    old empty backup until reserve/copy succeeds, then deletes the still-owned
    running frame through the existing allocation terminal.
    - Root diagnostics expose bounded one-shot denial for generator object,
      initial list, frame, post-growth yield snapshot and exception-frame
      growth. Initialized automation denies each twice, retries through real
      `task.spawn`/`ResumeTask`, forces collection after terminal rollback,
      and checks task removal, exact owner retirement/fatal drain, same-ID
      replacement and sibling continuity.
    - The GC root-work vector, normal-return StopIteration construction and
    ordinary exception/traceback allocation remain legacy blockers. Public
    constructors, normal/simple function calls and global allocation policy
    remain unchanged. See GENERATOR-STAGING-FALLIBLE.md. #153 remains
    incomplete.

33. #153 exact `StopIteration` slice: exhausted built-in iterators and normal
    generator return now use a private admitted transaction for the exact
    `tp_StopIteration` object. The transaction initializes `BaseException`
    fields directly and raises without a second allocation; a generator return
    copies its already-live return value directly into `StopIteration.value`.
    - ROOT diagnostics prove list/range `py_next == 0` ordinary exhaustion,
      preserved exact return value, and repeated exact-transaction
      forced-fresh-arena real-malloc denial with the exact owner fatal. Host
      automation carries return value `153` through
      `yield from`, then verifies task removal, no Python exception, fatal
      drain/accounting, sibling continuity, and a fresh normal-return
      generation. Ordinary successful completion still travels through the
      unchanged `py_next` 1/0/-1 protocol.
    - A returning generator has already consumed its frame before exception
      allocation. Therefore denial makes it inert (`state == 2`) as part of
      the noncatchable terminal unwind; leaving `state == 1` would permit a
      direct retry to dereference its null frame. It also clears the raw
      return value from `last_retval`, so no partial result retains a retired
      generation. This terminal-only state finalization is not a retryable
      Python result.
    - Generic exception construction, traceback/formatting, arbitrary Python
      `__next__`, the collector root-work vector, modules/import/cache, and
      production denial capability remain unchanged. See STOP-ITERATION-FALLIBLE.md.
      #153 remains incomplete.

34. #153 Mantle host-require transaction: catalog-backed `require()` module
    instances now carry explicit loading/ready/failed state. Only ready entries
    are reused; ordinary body failure is formatted under the existing policy,
    then unrooted/retired/removed so retry creates a fresh generation. A
    successful cache entry keeps its existing Python identity.
    - An exact module allocation terminal never enters exception formatting.
      A narrow per-VM allocation-free propagation latch carries terminal unwind
      through native `require()` after the scoped module cell restores its
      caller. It is not retirement authority. The outer host checkpoint takes
      the fixed VM/id/generation fatal, retires only that exact live module, and
      then clears the latch; caller and sibling generations remain live.
    - ROOT-only initialized automation separately clamps a real required module
      body and the admitted NameDict rehash in the real `py_trysetdict` cache
      publication. It proves failed/partially populated entries are never
      exposed as ready, owner-only retirement, caller/sibling survival,
      fresh-generation retry, successful cache identity, no stale latch, and
      ordinary exception remove/retry semantics. The denial seams and
      diagnostic generation receipts are absent from Shipping.
    - Generic `py_import`/cache publication, a general `ModuleTxn`,
      `IMPORT_STAR`, reload effects, the GC root-work vector, normal-return
      `StopIteration`, and ordinary traceback allocation remain out of scope.
    See HOST-REQUIRE-TRANSACTION.md. #153 remains incomplete.

35. #153 fallible Mantle host-value conversion: `PushVector`, `PushHandle`,
    recursive `PushValue`, callback tuples and signal-wake tuples use admitted
    constructors and stop on the exact current quota cell's allocation fatal.
    Arrays/maps stay unpublished until all children and map insertions succeed;
    recursive conversions reuse provisional duplicate-handle identities and do
    not publish them to weak caches/provenance until the outer value commits.
    - Additive `py_trydict_setitem` exposes the existing admitted dict insertion
      transaction for host map growth while legacy mutation remains available.
    - ROOT-only initialized automation denies Vector3, handle, managed string,
      nested map/list, fifth-entry dict growth, callback tuple and signal tuple
      production paths. A nested duplicate-handle/later-vector denial also
      verifies existing cache preservation, rollback to the cache/provenance
      baseline, and one-identity fresh retry. All cases verify one trigger,
      exact owner retirement, fatal and unwind drain, sibling survival, cache
      atomicity and no stale suspended or deferred work.
    - Root-table insertion is unchanged and remains separate work.
    See HOST-PUSHVALUE-FALLIBLE.md. #153 remains incomplete.

36. Linux/Clang dedicated-server toolchain compatibility (#163): the dedicated
    Ubuntu builder's Clang -Werror set caught three issues the prior Windows/
    MSVC and Epic-container Linux builds never surfaced.
    - `pk__try_name_key_txn` (item 13's admission helper) uses `INT_MAX` but
      the amalgamation never `#include`d `<limits.h>`; it happened to be
      visible transitively before. Added the include at that call site.
    - The `c11_color32` ANSI-wrapper diagnostic (item 8's follow-up) built
      five `(c11_color32){r,g,b,a}` literals with single braces; `c11_color32`
      is a union wrapping an `{r,g,b,a}` struct, so `-Wmissing-braces` (an
      error project-wide) flagged the elided inner brace. Double-braced them,
      matching the neighboring `c11_vec2`/`c11_vec3` literals already in that
      function.
    - Genuine upstream code: `Dict__try_get_index`'s `__RESTART:` label is
      directly followed by a declaration, legal only from C23; this module
      builds as C11 (`CStandard = CStandardVersion.C11` for the same reason
      QuickJSLib needs it, see PocketPyLib.Build.cs). Added an empty
      statement (`__RESTART:;`) after the label -- ordinary portable C11,
      no behavior change.
    No allocator, quota, GC, or Python-visible semantic change; this is pure
    toolchain compatibility for the Linux dedicated-server release build.

37. NameDict growth payer (#171): while a script runs, NameDict growth is
    admitted against the live current quota cell instead of the table's
    existing owner (`NameDict__live_growth_cell`, `NameDict__try_reserve_with`).
    - Why: the owner guards (`Frame__globals_has_current_owner`,
      `py__instance_dict_has_current_owner`, the IMPORT_STAR counterpart)
      sent stores into natively minted tables down the legacy setter. Under
      MANTLE_POCKETPY_ENFORCE_QUOTA that path already charged the running
      script through PK_MALLOC, but a denial dropped the store and reported
      success, so execution continued past it. OP_END_CLASS had no guard:
      admitting against a legacy numeric owner is refused with a fatal for
      generation 0, and the unwind dereferenced a nil exception.
    - Now: the guards are removed; a script-frame store into module globals,
      an instance __dict__, an IMPORT_STAR destination or an END_CLASS binding
      takes the admitted path whoever minted the table, and a denial is an
      exact, noncatchable terminal for the running generation. Size-overflow
      fatals follow the same payer. Accounting is unchanged: the running
      script already paid, and growth leaves the table owned by it, matching
      DICT-STORAGE.md's rehash rule.
    - With no live cell (host and bootstrap work) a natively minted table keeps
      the legacy setter; the NULL-safe `NameDict__set` fallback is retained but
      no quota denial can reach it.
    Covered by Mantle.Script.PythonSandbox.NativeOwnedTableFailureAtomicTerminal
    (instance attribute, global store, import star, class binding) through a
    diagnostics-only seam that denies one table's next growth on either path.
