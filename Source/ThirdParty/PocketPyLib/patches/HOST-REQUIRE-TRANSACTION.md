# Host `require()` failure transaction (#153)

This slice makes Mantle's catalog-backed PocketPy `require()` cache
failure-atomic without changing PocketPy's generic import machinery.

## Cache state and ownership

Each host module instance now has an explicit `Loading`, `Ready`, or `Failed`
state. Only `Ready` is a cache hit. `Loading` deterministically reports a
recursive require, and `Failed` is unavailable while a terminal unwind is in
flight. A successful entry retains its existing module/export identity.

An ordinary module-body exception is formatted and cleared under the existing
policy, then its failed instance is released and removed before `require()`
raises `ImportError`. Release unroots the module, retires connections and the
exact quota cell, and removes usage/fault ownership once. A later require
creates a fresh generation.

An exact allocation terminal has no Python exception and is never sent to
`py_formatexc()`. The entry is marked `Failed`; its exact instance remains only
until the outer `RunBudgeted` safe checkpoint takes the fixed POD fatal record.
That record's VM/id/generation/cell match is still the sole authority for
retirement. The checkpoint removes that module generation; caller, sibling,
and same-ID replacement generations cannot be selected by the unwind token.

## Nested terminal propagation

Nested module execution temporarily makes the module quota cell current.
`FScopedInstance` restores the caller cell before the native `require()` frame
returns, so the existing exact-current-cell predicate cannot by itself carry
the module terminal through the caller's interpreter frames.

PocketPy therefore has a per-VM, allocation-free, host-only nested-terminal
latch. Mantle arms it only after validating that the fixed fatal belongs to the
module cell. It is propagation state, not fatal authority: native dispatch and
the bytecode loop use it only to choose `RES_TERMINAL` instead of the ordinary
exception path. The latch is idempotent across nested return depths and is
cleared defensively at each outer resume entry and unconditionally at the outer
safe checkpoint immediately after the fatal record is taken. Ordinary errors
never arm it; VM reset also clears it.
This avoids exception fabrication, traceback allocation, catchability, and
retargeting the fatal to the restored caller.

## Diagnostic coverage

ROOT diagnostics add two one-shot per-VM seams. One clamps only a newly-created
required module generation during its real source compile/execute. The other
lets the body finish, then clamps the exact module immediately around the real
`py_trysetdict` cache-publication call. The seam pre-seeds `__export__`, then
adds private, pre-interned padding keys only while insertion is nonallocating,
until a ROOT-only read-only NameDict predicate proves the next distinct key
must grow the table. The body replaces the seeded export, and the predicate is
rechecked at the exact cache-write boundary before the quota clamp. This keeps
body completion allocation-independent and prevents a fixture/bootstrap count
change from silently moving denial to an earlier write. Neither seam is
compiled into Shipping.

Initialized automation performs real `require()` calls through both denial
points, observes terminal unwind and exact module retirement, proves caller and
sibling generations survive, retries through fresh generations, and verifies
repeated successful require identity. The publication fixture records that its
body ran twice, proving the failed partially populated module was not exposed
as `Ready` or reused. A separate real module raises ordinarily on its first body
run and succeeds on retry, making ordinary remove-and-retry behavior explicit.
The automation also checks that each unwind latch is drained.

## Deliberate exclusions

This patch does not convert or claim failure atomicity for generic `py_import`
or its cache publication, a general `ModuleTxn`, `IMPORT_STAR`, reload effects,
the GC root-work vector, normal-return `StopIteration`, or ordinary exception
and traceback allocation. Those remain separate #153 work.
