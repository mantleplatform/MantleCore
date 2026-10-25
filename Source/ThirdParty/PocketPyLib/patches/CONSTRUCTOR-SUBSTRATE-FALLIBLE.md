# Managed constructor substrate (#153)

This patch is an additive prerequisite, not a global allocator switch and not
completion of #153. Legacy `py_newstr` / `py_newstrn` / `py_newstrv`,
`c11_string__new` / `new2` / `new3`, `py_newobject`, and the `PK_MALLOC`
wrappers retain their existing non-denying behavior. Production call sites are
not silently changed to constructors whose failure they cannot observe.

## Explicit-result constructors

The existing admitted `py_trynewstrn` / `py_trynewstrv` pair remains the managed
Python-string constructor substrate. This patch adds admitted
`c11_string__try_new`, `c11_string__try_new2`, and `c11_string__try_new3` forms.
They validate signed sizes and complete allocation arithmetic, preserve the
caller output until all work succeeds, retain embedded NULs in the sized form,
and route quota, overflow, invalid-metadata, and backing failure through the
fixed exact-generation allocation-fatal channel. The formatted form uses the
already admitted/failure-atomic sbuf storage and a fallible counterpart to the
runtime's `%d/%i/%f/%s/%q/%Q/%v/%c/%p/%t/%n/%%` formatter. Attached storage is
destroyed on every failed write or submit; the detached string is published
only after submit succeeds.

`py_trynewobject` is the additive managed-object entry point. It preserves both
the TValue and userdata outputs on failure. It accepts only a registered
concrete type in the same exact range as `pk_typeinfo` (`tp_nil < type <
current types.length`) before allocation. Callers remain responsible for
supplying a slots/userdata layout compatible with that registered type's runtime
use and destructor; the generic allocator cannot infer or validate that semantic
layout. `ManagedHeap__try_gcnew` now accepts
the runtime's logical `slots == -1` shape: it first admits and zeroes the
four-entry `NameDict` table, then admits the managed object. If object/arena
allocation fails, the table is freed before returning and the object, arena,
GC counter, and caller outputs remain unpublished. After both allocations
succeed, embedding the already-owned table cannot fail. Sweep and destruction
free the table through its retained allocation header, so an exact original
owner is debited even when a sibling generation is current.

Later NameDict rehash is deliberately unchanged and still uses legacy
`PK_MALLOC`. This patch therefore makes construction failure-atomic; it does not
claim that arbitrary attribute mutation is hard-capped.

## Exact caller ledger

The following direct legacy `c11_string` sites remain to be transactionally
converted before legacy wrappers can deny:

- `SourceData__ctor` filename ownership;
- the token/compiler temporary used while materializing string tokens;
- `FuncDecl__ctor` / CodeObject function-name ownership;
- ModuleInfo package and path construction;
- all six import/cache filename formats (`.py`, `.pyc`, and package
  `__init__` variants across source and bytecode lookup);
- traceback frame-name capture;
- the legacy `c11_string__copy` and one-character `c11_sv__replace` helpers.

The supported/reachable `slots == -1` legacy object callers likewise remain on
`py_newobject`: generic Python instance creation, type construction/reload, and
module/bootstrap/cache creation. Each needs a caller-owned transaction that can
return the existing terminal result before `py_newobject` itself can be switched.
ModuleInfo is especially coupled: its object, NameDict, package/path strings,
module tree insertion, and dict attributes must publish or roll back as one unit.
The OS path-object caller is compile-excluded by `PK_ENABLE_OS=0`; dynamic pickle
deserialization is compiled but policy-blocked. Neither is a supported-reachable
production caller in this inventory.

The next safe production slice is therefore the SourceData/compiler name
ownership chain, followed by the ModuleInfo/import-cache transaction. The host
`PushValue` `py_newstr` sites, name/type caches, serializers, error/traceback
formatting, and remaining public legacy string calls are separate ledgers.

## Diagnostics and current inventory

Non-Shipping root diagnostics use only modest real allocations. They cover
inline and managed Python strings, small/sized/formatted c11 strings, embedded
and trailing NULs, formatted import-style paths, injected backing failure,
twice-repeated quota denial for all three c11 forms, retry/reuse, unchanged
outputs/retval/SP and committed/reserved accounting, and exact terminal
identity. Managed-heap diagnostics additionally cover twice-repeated first
allocation denial, twice-repeated NameDict second-allocation backing failure,
rollback without arena/object/GC publication, successful retry, exact table
ownership, sweep, and teardown while a sibling is current. The initialized
public diagnostic repeats `py_trynewobject(slots == -1)` denial while preserving
both outputs, retval, and SP. It also rejects zero, negative, and upper-bound
types before fresh-small-pool, reused-small-pool, and large-object shapes,
proving unchanged heap tracking, outputs, retval, SP, and accounting. Formatter
success checks exercise every supported custom specifier, not only the `%s/%c`
forms currently needed by import filenames. The public `tp_nil/slots == -1`
case specifically proves validation precedes NameDict table admission; the
fresh-pool case uses a local empty heap and is independent of test order.

Current vendored/build inventory corrects an older bytes ledger: LZ4 and the
binary-IO byte-result path are not present in the active build. Pickle and stdc
remain policy-blocked capabilities rather than reachable production conversion
targets. Their compiled source/legacy definitions are not evidence of runtime
reachability. Broad production `PK_MALLOC`/`PK_REALLOC` remains soft until every
reachable caller has an explicit terminal-safe contract.
