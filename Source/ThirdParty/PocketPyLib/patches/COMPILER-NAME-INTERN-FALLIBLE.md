# Compiler-only fallible name interning (#153)

The admitted public in-memory compiler now interns first-seen source identifiers
and its fixed grammar names `self` and `_` through an explicit-result path. It
checks `NameBucket` payload arithmetic, looks up under the process-global table
lock, releases the lock before quota admission/allocation, then rechecks before
publication. A racing candidate is freed outside the lock. No allocator
callback, fatal publication, or cleanup runs while the name lock is held.

A published name bucket lives until `pk_names_finalize`, longer than any script
instance quota cell. The allocation is admitted against the compiling exact
generation, then an accounting-only ownership transfer clears that temporary
cell pointer while retaining the allocation's VM charge. Finalization can
therefore debit the stable VM total without dereferencing a retired cell.
Legacy `py_name` / `py_namev` callers and their behavior are unchanged.

Denial latches the existing allocation-independent exact-generation terminal in
`CompilerTxn`; output `CodeObject`, retval, stack and compiler-owned accounting
remain unchanged. `FuncDecl.recursive_name` is obtained before declaration
allocation/publication, so named functions and lambdas retain the same interned
name used by their `CodeObject` after retry. Runtime attribute APIs, class/type
runtime construction, import path buffers/runtime module loading, modules,
serialization/deserialization and global quota policy remain outside this
slice. Source class identifiers, normal-import bindings/aliases, from-import
attributes, and from-import bindings/aliases are all admitted here.

The compiler-region audit leaves only the explicit non-admitted
`CompilerTxn__try_namev` compatibility branch. The fallible constructor stages
both fixed names before `CodeObject` or context publication, retains them in
the compiler/context state, and uses the stored underscore name for discard
emission. CodeObject/FuncDecl reconstruction remains in the separate
deserialization path. No admitted `_py_compile` name path calls `py_name` or
`py_namev` directly.

Non-Shipping diagnostics cover payload-size overflow, repeated empty-bucket
denial and retry, a real hash-collision chain, named-function denial, nested
lambda denial, every class/import name route, and a deterministic second-lookup
race which publishes a competitor then proves the losing candidate is freed.
They also substitute fresh, process-globally absent private spellings for the
two fixed constructor slots, independently deny/retry `self` and `_`, and
verify that the denied target has no published bucket while output/SP/retval,
exact-cell accounting and the fatal identity remain unchanged. They cover
chain/output/FuncDecl/accounting preservation, detached process ownership,
exact-owner name-intern terminal identity and sibling continuity. The host
terminal fixture performs a bounded deterministic scan for a process-globally
absent diagnostic name, then arms denial atomically with the final locked
absence proof. It allocates and invokes no callback while the name-table lock is
held, so repeated fixtures in one process cannot silently reuse an already
interned name and bypass the `NAME_INTERN` checkpoint.

Successful diagnostic retry compiles inspect their unpublished `CodeObject`
before its compiler transaction immediately discards tracked managed constants
and backing arenas. This keeps the retry ownership proof exact without relying
on a later GC; normal compiler success continues to commit and publish those
values unchanged.

The diagnostic prewarms the ordinary `self` and `_` spellings through the same
admitted helper with no instance cell before taking its baseline. A missing
process-lifetime bucket is therefore VM-owned from creation, rather than an
incidental legacy prewarm leaving a permanent charge on the live test cell.
