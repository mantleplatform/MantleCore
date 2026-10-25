# Managed-heap admitted tracking prerequisite (#153)

This patch adds storage machinery needed before managed Python string constructors
can safely opt into hard quota admission. It does not switch a production caller,
does not change `ManagedHeap__gcnew`, and does not make `PK_MALLOC` denying.

## Two tracking domains

Legacy small arenas remain in `Pool.arenas`; legacy large objects remain in
`ManagedHeap.large_objects`. Their aborting vector contracts are unchanged.
Objects created by the internal `ManagedHeap__try_gcnew` use parallel intrusive
sets:

- each admitted `PoolArena` carries a `mantle_next` link and is published only
  after the admitted backing allocation and complete arena initialization;
- each admitted large object follows a co-allocated `ManagedHeapLargePrefix`.
  The union contains a `py_TValue` alignment carrier, and compile-time
  assertions require its alignment and size to preserve `PyObject`/`py_TValue`
  alignment on Win64 and Linux without depending on optional `max_align_t`
  exposure in the C library headers.

There is no vector reserve, map access, Python API, logger or host callback in
admission/tracking. Allocation failure leaves the caller output, GC counter,
large totals/counts, arena lists/counts and pool contents unchanged. Quota denial
therefore happens before libc; libc-equivalent failure is rolled back by the
shared admitted allocator before any heap publication.

## Collection and ownership

Small sweep scans both arena sets. The intrusive set retains the same effective
policy as the legacy sorted vector: every nonempty arena, at least half of the
previous set, and at least one arena beyond the full set. Allocation scans for a
nonfull admitted arena, so intrusive node order is not semantic. Large sweep
unlinks dead prefixes without allocation. Destruction visits both domains.

The prefix/arena pointer given to `PK_FREE` is the exact admitted allocation user
pointer. Its private Mantle header retains the original quota-cell generation;
freeing while a sibling is current debits that original cell. Approximate
PocketPy heap reporting continues to count 120 KiB per arena and quantized object
bytes, while the quota layer charges the complete physical allocation including
metadata and private header.

## Follow-up boundary

The constructor-substrate follow-up now admits `slots == -1` by acquiring the
initial NameDict table before the object and rolling that table back if the
object allocation fails. Checked arithmetic still rejects `slots < -1`, negative
`udsize`, slot multiplication overflow, header/payload overflow and any object
size outside the positive-int domain consumed by the size codec/pool index.
Later NameDict rehash and all legacy `py_newobject` callers remain unchanged;
see `CONSTRUCTOR-SUBSTRATE-FALLIBLE.md`.

`py_newstrn`/`py_newstrv` still call legacy `ManagedHeap__gcnew`. A later bounded
slice adds separate `py_trynewstrn`/`py_trynewstrv` entry points and converts a
closed native leaf set; it does not change these legacy constructors or enable a
global denying allocator. See `MANAGED-STRING-FALLIBLE.md`.

## Deterministic diagnostic

`py_mantle_managed_heap_storage_test` is compiled only with root diagnostics and
runs after a VM is initialized. It saves/restores VM quota/fatal state and uses a
local stable cell plus a call-local backing allocator. Modest fixed allocations
cover negative/overflow rejection, twice-repeated arena and large quota denial,
twice-repeated backing failure and rollback, successful arena/large publication,
absence of legacy tracking-vector entries, exact header owner/generation, and
cleanup while a sibling cell is current. It does not allocate a huge valid
payload, replace a process allocator, execute script code, or enable production
denial.

The follow-up adds repeated first-allocation denial, repeated injected
NameDict-second-allocation failure, successful retry, exact NameDict-table
ownership, and sibling-current dict/object sweep and teardown.
