# Lexer queue and token-payload transaction (#153)

## Converted boundary

The in-memory public compile path (`py_compile`, `py_exec`, and `py_eval`) now
selects the admitted lexer mode after `SourceData__try_rcnew` succeeds. The
transaction owns two unpublished vectors, for `Token` and indentation levels,
and every `c11_string` payload attached to a string, bytes, or f-string token.
Initial reserve and later growth use `MantlePocketPy_TryAdmittedVectorReserve`;
literal buffers use the admitted fallible sbuf ctor, writes, and submit.

Each push plans before mutation. A failed reserve leaves vector fields and bytes
unchanged. A payload whose token push fails is deleted immediately; payloads
already queued are deleted once by transaction rollback. Attached sbufs are
destroyed on write/submit failure. Only a complete EOF token stream submits its
token allocation to the compiler. Both output arguments therefore remain
unchanged on denial, and all committed/reserved charges return to the entry
baseline. Allocation failure remains exception-free and uses the existing fixed
fatal record for the exact quota-cell generation.

Ordinary lexical failures still allocate and return the existing `Error`, become
`SyntaxError` in `_py_compile`, and retain their source frame behavior. Source
normalization and pre-sized line-start recording are unchanged.

## Compatibility boundary

`py_newfunction` native-signature compilation retains `pk_compile`'s legacy
aborting lexer mode. This avoids extending the conversion into VM bootstrap,
module/type registration, or native binding construction. The admitted public
mode stops after tokenization: parser AST allocation, Compiler contexts,
CodeObject/codegen vectors, function declarations, names/constants, imports,
traceback/error formatting, serialization, PushValue, and generic allocator
policy remain outside this slice. Production allocator denial is not enabled
globally.

## Diagnostics

The non-Shipping root diagnostic uses local lexer checkpoints and real quota
admission. It denies each reachable allocation stage twice: initial token,
initial indent, token growth after an owned string payload, indent growth,
payload buffer construction, payload growth, and submit-NUL growth. Every case
checks untouched outputs, retval and stack, exact fatal identity, unchanged
committed/reserved VM and generation accounting, rooted sibling identity, and a
successful reusable retry whose vector and payload headers name the live owner.
An ordinary mismatched-indentation case returns an `Error` with no fatal.

One root-only, one-shot checkpoint clamps the current exact owner's quota at the
first lexer allocation reached through real `py_exec`. Host automation verifies
that the terminal drains through the normal checkpoint, retires only that owner,
and leaves a sibling generation able to compile and execute again. The seam is
compiled out of Shipping and does not replace an allocator or expose a script
capability.
