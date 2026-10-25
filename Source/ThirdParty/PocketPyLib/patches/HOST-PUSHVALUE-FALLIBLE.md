# Host PushValue fallibility (#153)

Mantle's PocketPy host bridge now treats native-to-Python value conversion as
an exact-owner terminal transaction.

- `PushVector`, `PushHandle`, and recursive `PushValue` return `bool` and use
  the admitted object, string, list, and dict constructors. They stop as soon
  as the current quota cell owns an allocation fatal.
- Arrays and maps are assembled under stack roots and assigned to the caller's
  output only after every child succeeds. A recursive conversion keeps new
  handle wrappers in an outer transaction, reuses one provisional identity for
  duplicate handles, and publishes its weak-cache/provenance entries only when
  the complete outer value commits. Failure leaves pre-existing entries intact.
- `py_trydict_setitem` exposes the existing admitted, failure-atomic
  `Dict__try_set_admitted` transaction for this explicit host caller. Dict
  storage growth and rehash preserve the original allocation owner and leave
  the unpublished temporary unchanged on denial.
- Getter, method, require fallback, callback, and signal-wake callers propagate
  failure without manufacturing or clearing an ordinary Python exception.
  Callback and signal argument tuples are admitted before publication; direct
  host entries drain the fatal only after the scoped owner context unwinds.
- ROOT diagnostics deny real initialized-host Vector3, handle, managed-string,
  nested map/list, dict-entry-growth, callback-tuple, and signal-tuple paths.
  A nested duplicate-handle case then denies a later vector and checks rollback
  against a live cached identity plus fresh-retry identity/accounting. Together
  they check one-shot trigger retirement, exact owner retirement, fatal/latch
  drain, weak-cache atomicity, sibling survival, and suspended/deferred cleanup.

Root-table insertion is deliberately unchanged. Other legacy public
constructors and general host-root publication remain later #153 work.
