# First admitted managed-string caller slice (#153)

This is a bounded production caller conversion, not a global hard cap. Legacy
`py_newstr`, `py_newstrn`, `py_newstrv`, `py_newfstr`, and `PK_MALLOC` remain
non-denying. The additive `py_trynewstrn` / `py_trynewstrv` pair is used only by
the closed native leaves listed below.

## Constructor contract

`py_trynewstrn(out, size, out_data)` preserves both outputs on failure. Sizes
0-15 use a zero-initialized temporary `py_TValue`, publish it atomically, and
return its inline data span. Size 16 is the first managed representation. Its
complete `c11_string` payload and trailing NUL are checked in `size_t` and the
positive-int managed-heap domain before `ManagedHeap__try_gcnew` is called.
Managed object initialization is non-fallible after admission; only then are the
value and byte pointer published.

`py_trynewstrv` first constructs into a local value, copies only after try
success, and publishes the caller output last. Embedded NUL bytes are ordinary
payload; both representations retain the extra trailing NUL. Invalid metadata,
overflow, quota denial, and backing failure use the fixed POD allocation-fatal
channel. Neither function creates a Python exception, logs, invokes a host
callback, or changes the Python stack.

The admitted managed-heap backend tracks arenas/large objects without a tracking
allocation. It retains exact physical allocation-header ownership. Reusing an
already admitted arena does not create or charge new physical storage; large
objects remain dedicated allocations and are used by deterministic denial tests.

## Converted production leaves

The exact converted set is:

- `int__repr__` and `builtins_chr`;
- `str__add__`, `str__mul__`/`str__rmul__`, `str_lower`, `str_upper`,
  `str__strip_impl` (`strip`, `lstrip`, `rstrip`), and the integer result branch
  of `str__getitem__`;
- `bytes_decode`;
- final publication in `c11_sbuf__try_py_submit`, covering the already converted
  non-recursive repr/string-padding family and `str.join` documented in
  `STRING-BUFFER-FALLIBLE.md`;
- final publication for `str.replace` and the slice branch of
  `str.__getitem__`, after their admitted exact-size detached helpers complete.

Direct destination copies/case loops begin only after try allocation succeeds.
Sbuf submit always deletes its detached storage after the managed-string attempt;
denial leaves the output unchanged and the caller's subsequent buffer destructor
is a no-op. Every leaf returns ordinary `false` into the existing exact-generation
`RES_TERMINAL`, so creator `try/except` cannot consume quota denial.

## Explicit exclusions

The following remain legacy even when they eventually produce strings:

- `str.split`, `splitlines`, `%`, `format`, and string iterator next;
  aggregates, nested dispatch, iterator state, or temporary ownership need
  separate transaction audits;
- `py_name2ref`, name/type/module/package caches and attributes, compiler/lexer/
  module creation, imports, Host `PushValue`, source/error/traceback formatting,
  serializers/deserializers, and import-denied modules;
- generic/recursive repr families still on legacy sbuf operations, direct
  `c11_string` constructors, `py_newfstr`, unconverted bytes constructors, and
  all `slots == -1` managed objects. The later closed core bytes subset is
  inventoried separately in `MANAGED-BYTES-FALLIBLE.md`.

No failure path constructs `MemoryError`; the terminal host still reports and
retires only the exact generation at its outer safe checkpoint. Unconverted
callers and globally soft allocator wrappers are why #153 remains incomplete.

The constructor-substrate follow-up adds admitted `c11_string__try_new/new2/new3`
without switching their legacy wrappers or callers. Its exact compiler/import/
cache caller ledger and diagnostics are in `CONSTRUCTOR-SUBSTRATE-FALLIBLE.md`.

## Coverage

The non-Shipping storage diagnostic checks inline 15-byte and managed 16-byte
success, embedded and trailing NULs, unchanged retval/SP/value/data-pointer on
deterministic direct and submitted large managed denial, exact fatal ownership,
and release of submitted sbuf storage back to the original quota cell. The
initialized automation exercises
ordinary semantics for every converted direct leaf, then denies a modest
160-byte `bytes.decode` result inside a nested native call. The terminal is not
catchable, retires only its owner, and a sibling generation continues.
The join follow-up exercises a dedicated managed publication after fully
consuming and popping its iterator; denial preserves the exhaustion retval,
releases detached sbuf storage, records the exact generation, and does not
disturb a sibling stack sentinel.
The detached-string follow-up injects backing failure before replace/slice
helper publication and denies a modest dedicated managed publication after the
temporary is complete. Both preserve outputs and exact fatal ownership, release
all temporary storage, and retain a healthy sibling. Its bounds-checked linear
span walker also preserves every arbitrary byte admitted by `bytes.decode`,
treating stray continuation, invalid-lead, and truncated runs as one-byte units
without rescanning from the beginning or reading beyond the payload.
The shared legacy UTF-8 length/getitem helpers are intentionally unchanged;
malformed-byte integer indexing and their strip/module callers remain a later
semantic-audit boundary.
