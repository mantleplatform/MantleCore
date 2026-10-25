# SourceData fallible compiler-entry transaction (#153)

## Converted ownership transaction

`SourceData__try_rcnew` is the explicit-result entry used by `_py_compile`.
It preserves the caller's output until all of these allocations succeed:

1. the refcounted `SourceData` object;
2. its filename `c11_string`;
3. the admitted `c11_sbuf` used to normalize source bytes, including any growth
   and the trailing NUL appended by fallible submit; and
4. a `const char*` vector sized for the first line plus every normalized newline.

Construction initializes private members first. Every failure destroys the
attached sbuf or submitted source, filename, line vector, and owner in reverse
ownership order. Only success stores the caller's `SourceData_`. Allocation
failure is communicated solely by the fixed, allocation-independent fatal record
already owned by the current quota-cell generation; `_py_compile` does not try to
format a Python exception on that path.

Normalization intentionally preserves upstream semantics. A UTF-8 BOM at the
start is skipped, every CR is dropped, trailing `isspace` bytes are trimmed when
a non-space byte remains (the upstream all-whitespace edge case is unchanged),
the first input NUL terminates the C string, and the owned result always has a
trailing NUL. A null source remains valid for bytecode metadata and allocates no
source or line vector.

## Publication and reachability

`py_compile`, `py_exec`, and `py_eval` all enter `_py_compile`, so SourceData
denial returns false with no exception and lets the existing exact-generation
host checkpoint select terminal unwind. Successful construction still calls the
unchanged lexer and compiler. Ordinary lexer/compiler errors still become a
`SyntaxError`, retain their source snapshot, and remain catchable/clearable.
The host's compile-only `MantlePocketPyCheckSource` entry explicitly runs as the
cell-less owner and drains only that exact fatal before creating its fixed
host-side validation message; it never formats a nonexistent Python exception.

`py_execo` does not compile source. It deserializes a `CodeObject`; that path and
`py_newfunction`'s native-signature compiler still use the compatibility
`SourceData__rcnew` wrapper. The wrapper now shares the admitted constructor but
retains its historical abort-on-failure contract because their full callers are
not converted in this slice. The optional OS `py_compilefile` reader also keeps
its legacy input/output buffers, although its in-memory compile step now enters
the converted `_py_compile` path.

## Diagnostic contract

Non-Shipping root diagnostics use a local checkpoint, never a process-wide
allocator override. They impose real quota denial twice at the object, filename,
initial normalization buffer, normalization growth, submit-NUL growth, and
line-start stages. Local malloc callbacks also inject backing failure after real
reservation for the owner and filename. Every case checks unchanged output,
retval, stack pointer, rooted sibling, committed/reserved accounting, exact fatal
identity, and reusable retry. Success checks normalized bytes, embedded input
NUL termination, null-source behavior, and the original generation on all four
allocation headers. The host automation additionally verifies ordinary syntax
errors preserve the generation and a compiler-entry denial retires only the
owner while its sibling continues.

## Remaining boundary

The complete line-start capacity is admitted here, so lexer newline recording
cannot grow that vector. The follow-up in `LEXER-QUEUE-FALLIBLE.md` converts the
public compile path's token/indent storage and token-owned literal strings.
Compiler contexts, CodeObject vectors, function declarations, names/constants,
exception formatting, serialization/deserialization, and general `c11_vector`
operations remain legacy. They use the globally soft PK allocator or aborting
vector wrappers and therefore cannot yet participate in hard denial safely.
Neither `PK_MALLOC`/`PK_REALLOC` nor `c11_vector__push` is globally switched.
Those caller graphs require separate ownership transactions before the cap can
be globally denying.
