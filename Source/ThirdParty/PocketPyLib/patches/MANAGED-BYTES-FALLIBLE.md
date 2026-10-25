# #153 admitted bytes-family slice

This is a bounded production caller conversion, not completion of the hard
memory cap. Global `PK_MALLOC` / `PK_REALLOC` remain non-denying, and the legacy
`py_newbytes` entry point remains for explicitly excluded callers.

## Converted contract

`py_trynewbytesn` validates the signed payload and complete
`PyObject + c11_bytes + data` layout before calling the admitted managed-heap
allocator. The bytes header and payload share one managed allocation; there is
no separately owned native backing store. Allocation denial or backing failure
leaves both output TValue and output-data pointer unchanged. Success initializes
the size and publishes both outputs only after the managed object exists.

The converted enabled core family is exactly:

- `bytes()` for the empty and existing list/tuple forms. List/tuple elements are
  type- and range-checked before allocation, so ordinary `TypeError` /
  `ValueError` cannot expose a partial object.
- `str.encode()`, preserving the explicit byte length and embedded NULs.
- `bytes.__add__`, with checked exact result sizing and local fill before retval
  publication. A non-bytes operand still returns `NotImplemented`.
- the slice branch of `bytes.__getitem__`. It derives the exact normalized slice
  count with widened arithmetic, allocates once, and fills in one traversal.
  The former aborting temporary byte vector is removed. Integer indexing and
  ordinary slice errors are unchanged.
- `bytes.decode()`, which was already routed through `py_trynewstrv`. It remains
  part of this closed family and intentionally performs no UTF-8 validation:
  continuation-only, truncated, invalid-lead, and embedded-NUL bytes are copied
  unchanged into PocketPy's explicit-length string.
- interpreter `OP_BUILD_BYTES` publication. The bytecode constant remains an
  explicit-length string owned by the unchanged compiler. Execution allocates
  and fills a local bytes TValue, then advances the operand stack only after the
  copy completes. Denial reaches the existing `__ERROR` discriminator with no
  exception and selects exact-generation terminal unwind.
- `base64.b64encode` and `base64.b64decode`. Both allocate through
  `py_trynewbytesn`, fill and resize a local bytes TValue, and publish retval
  only after the transform completes. Encode capacity arithmetic is widened
  before narrowing; existing decode acceptance and ordinary type errors remain
  unchanged.

PocketPy v2.2.0 in this vendoring has no bytes `__mul__` / `__rmul__` binding or
repeat implementation. This patch does not add one: doing so would enable a new
runtime capability rather than harden an existing allocation path.

False from any converted allocation creates no Python exception and propagates
through the existing exact-generation terminal result. The result object is
kept in a local TValue while bytes are copied, so retval is unchanged until the
operation is complete. There is no native temporary ownership after the slice
conversion; denied admitted allocations roll their reservation back inside the
managed heap.

## Deterministic coverage

`py_mantle_bytes_storage_test` uses only modest payloads. A 600-byte request is
large enough to take the managed heap's coallocated large-object path. An
injected allocator returns null after real admission/reservation; the diagnostic
checks one backing call, unchanged output TValue/data pointer, unchanged retval
and stack sibling, exact `AdmissionAllocFailed`, unchanged committed charge, and
zero reservation. This is a caller-local allocator callback, not global
production denial. A separate real quota clamp denies the same helper before
managed object/payload allocation and leaves the exact `AdmissionInstanceLimit`
record for the host to validate against the live generation. A normal four-byte
allocation succeeds after the injected failure.

The initialized automation pre-creates two bytes operands, clamps only the
caller's quota, and invokes real bytes concatenation through a nested callable.
Creator `try/except` cannot continue; the exact owner is retired, its sibling
generation survives, and the terminal record is drained. The sibling then
checks empty/list construction, embedded-NUL/0xff encode and concat, positive,
negative and stepped slices, malformed decode/encode byte preservation, and
the existing ordinary constructor/range/zero-step exceptions.

The byte-literal follow-up compiles a modest literal function before entering a
native diagnostic boundary, then clamps quota only around its nested execution.
The real `OP_BUILD_BYTES` allocation is denied without advancing its operand
stack. The boundary verifies restored SP, unchanged nil retval, an intact stack
sibling, unchanged committed/reserved accounting, and the exact active-cell
fatal before returning into terminal unwind. Automation proves the creator's
handler cannot continue, only the owner generation retires, and a sibling still
constructs embedded-NUL and malformed byte literals normally.

The base64 follow-up pre-creates separate encode/decode inputs whose result
allocations exceed the managed heap's small-pool cutoff, then clamps quota only
around each real module call. Both paths preserve SP, nil retval, stack sibling,
and instance/VM committed-reserved accounting while recording the exact active
cell fatal. Each terminal retires only its owner; a sibling generation then
checks binary round trips, string/bytes decode inputs, invalid-data behavior,
and ordinary type errors.

## Remaining immediate callers and gaps

`py_newbytes` remains the explicitly legacy compatibility entry point. Current
vendored/build inventory shows that LZ4 and the binary-IO byte-result path are
absent from the active build. Pickle bytes load/output and `stdc_read_bytes`
remain policy-blocked capabilities, not reachable production callers.

The legacy `py_newbytes` definition and compiled optional source still remain;
they must not be inferred to be hard-capped by the additive try API.
General list/dict/object construction, format/repr/traceback, host `PushValue`,
and capability enablement are also unchanged. These gaps, the legacy
constructor, and the globally soft allocator are why #153 remains open.
