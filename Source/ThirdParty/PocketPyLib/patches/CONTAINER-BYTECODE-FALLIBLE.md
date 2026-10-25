# Container bytecode failure-atomic slice (#153)

Only `BUILD_TUPLE`, `BUILD_LIST`, `BUILD_DICT`, and `BUILD_SLICE` use the new
admitted constructors.  Each constructor leaves its public output unchanged on
failure; the bytecode handlers retain the original operands and delay stack
replacement until the value is complete.

Dict literals are special because `__hash__` and equality can run Python.  An
unpublished `ManagedHeapObjectTxn` owns the object and its dictionary backing.
The object is temporarily rooted on the VM stack during each callback, then
that private root is removed before the opcode commits the transaction and
publishes its result.  Operand and root addresses are reacquired after each
callback; dictionary vectors or entries are never retained across one.

Transactions record the heap collection epoch which created their one
`gc_counter` credit. Explicit and automatic collections advance that epoch
before resetting the counter (including their hooks/debug callbacks). Thus a
no-GC discard still subtracts exactly its own allocation credit, while a
post-GC discard subtracts nothing from allocations made after the reset. The
temporary VM-stack root keeps the provisional dict alive during callback-time
sweeps. `PK_GC_TXN_PINNED` is armed later, only while discard runs an armed
destructor, and protects that destructor-time nested-sweep window.

The private admitted Dict route allocates its combined entries/index storage
against the original allocation owner.  It prepares entry capacity and rehash
storage before length/index publication, so a denied backing allocation leaves
the literal's transaction discardable and its operands untouched.  Existing
`py_new*`, `py_newdict`, `py_dict_setitem`, and ordinary dict behavior remain
unchanged.

An admitted dict probes once. If that absence requires rehash, it uses the
first cached hash to locate the new empty slot without invoking `__hash__` or
`__eq__` again; stateful collision callbacks therefore retain ordinary
single-probe semantics. Public `py_trynewlistn` also clears each admitted
element before committing its visible length.

`py_mantle_container_bytecode_storage_test` is non-Shipping coverage for
repeated object and backing denials plus dict entry-growth and rehash retry;
each attempt checks exact committed/reserved restoration, unchanged output and
dict metadata, and the successful temporary cleanup returns to baseline. The
initialized `Mantle.Script.PythonSandbox.ContainerBytecodeFailureAtomic`
automation executes a forced-GC failing hash in a real dict literal, verifies
the exact stateful collision callback count through the rehash threshold and
duplicate replacement, then uses a narrow diagnostics-only admitted fail-site
marker to prove and deterministically deny each production
`OP_BUILD_TUPLE`, `OP_BUILD_LIST`, `OP_BUILD_DICT`, and `OP_BUILD_SLICE` after
its callable is compiled. Those terminal paths check stack/retval/operands and
quota restoration, retire only the exact owner, and leave a sibling live.
