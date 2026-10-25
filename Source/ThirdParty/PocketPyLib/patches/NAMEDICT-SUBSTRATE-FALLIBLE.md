# NameDict and unpublished-object substrate (#153)

This additive slice supplies ownership machinery needed by later compound
constructors. It does not convert any production constructor or change the
legacy `py_setdict` / `py_emplacedict` behavior.

## Exact-owner growth

`MantlePocketPy_TryAdmittedMallocLike` admits a distinct allocation against an
existing admitted block's retained quota-cell pointer. NameDict rehash uses that
primitive because open-addressed entries cannot be safely grown in place. A
retired original cell therefore rejects growth with an exact-generation fatal;
the operation never borrows the current sibling's quota. Numeric legacy-owner
blocks are rejected rather than silently changing their debit route.

The internal NameDict reserve/rehash/set path allocates and completely rebuilds
a replacement table before swapping it into the dictionary. Denial leaves the
old table, length, capacity, mask, values and accounting unchanged. Existing-key
replacement remains allocation-free. Public additive `py_trysetdict` and
output-preserving `py_tryemplacedict` expose this contract for later converted
callers; the original APIs still use the non-denying legacy path.

## Unpublished managed objects

`ManagedHeapObjectTxn` explicitly owns one admitted object between allocation
and publication. Commit relinquishes rollback authority. Discard verifies the
same backing allocation owner/generation, decrements the GC counter once,
unlinks admitted large objects or returns the exact small block, runs the type
and NameDict destructors once, and releases a transaction-created arena only
when it is still empty. The generic NameDict prerequisite is always destroyed;
the type-specific userdata destructor is armed explicitly only after userdata
construction completes, so partial construction never invokes a destructor on
garbage. Large objects are unlinked before destruction. Small objects carry a
dedicated internal transaction pin while their destructor runs. Every sweep
retains that pin while clearing only the ordinary one-cycle mark, and
`gc.track()` preserves it; only the owning transaction clears the pin after the
destructor returns. Any number of destructor-nested collections therefore
cannot destroy or return the same block before the outer discard does so once.

The compiler's pre-existing string/tuple rollback now shares this generic
discard primitive while retaining its compiler-wide arena snapshot. Public
`py_trynewobject` uses a transaction internally and commits immediately, so its
observable contract is unchanged.

## Diagnostic and exclusions

The focused non-Shipping diagnostic repeats initial NameDict denial and both
try-set/try-emplace rehash denial twice, retries successfully, and checks absent
attributes, output preservation, stack/retval/accounting invariants, exact
retired-owner rejection, sibling non-charging, late debit, and ordinary
nonallocating replacement. A custom armed type destructor also invokes full GC
twice and proves one destructor call, one block return, no duplicate unused
index, exact pool/arena/counter/accounting restoration, and SP/retval
preservation. Its host wrapper additionally distinguishes an ordinary
`AttributeError` from the allocation-terminal channel and confirms both live
generations survive.

List, tuple, dict, slice and type constructors are not converted. The bounded
`OP_LOAD_FUNCTION` closure follow-up is documented separately in
`FUNCTION-CLOSURE-FALLIBLE.md`; public/native function constructors remain
legacy. Modules, imports and caches; host PushValue, roots and callbacks;
errors and tracebacks; global denial; and production capabilities are unchanged.
