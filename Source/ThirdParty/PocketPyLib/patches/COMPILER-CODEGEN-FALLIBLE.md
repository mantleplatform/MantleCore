# Compiler/codegen vector transaction (#153)

The in-memory public `py_compile` / `py_exec` / `py_eval` path now extends the
admitted compiler-entry transaction beyond the lexer. CodeObject name and root
block construction, compiler contexts, blocks, paired bytecode/metadata,
constants, function-declaration objects and publication, plus CodeObject and
FuncDecl var/name/arg/kwarg vectors use explicit-result admission.

Bytecode and line/block metadata are planned together and publish lengths only
after both reservations succeed. A compiler transaction latches the first
denial; emit and patch operations then become inert and existing parser call
boundaries unwind without constructing an exception. The unpublished
CodeObject is destroyed, tokens and contexts are released exactly once, and
caller output, retval, stack, VM accounting, and exact-owner accounting remain
unchanged. Retry publishes only after the whole transaction completes.

The admitted constant-string path uses a bounded linear lookup over CodeObject
constants instead of the legacy compiler small-map. This avoids separately
owned raw map keys inside the converted transaction while preserving constant
deduplication. The later bounded follow-up in
`COMPILER-SMALLMAP-FALLIBLE.md` converts only the admitted compiler's
global/inverse-name/keyword maps; the generic smallmap remains legacy.

Managed strings and default-argument tuples are not owned by the native vectors;
they enter the VM heap before CodeObject publication. The transaction therefore
tracks each newly created managed value. Failure destroys exactly those
unpublished values, returns their small-pool blocks, unlinks large objects,
releases any arenas first created by the transaction, and restores the heap GC
counter. If allocation of a tracking record itself is denied, the single newest
value is retained in the transaction latch for the same rollback. Successful
compilation drops only the tracking records and leaves the now-published values
under normal GC ownership. This is compiler-local and does not change generic
heap collection or constructor policy.

Native-signature compilation and CodeObject deserialization retain legacy
constructors. The follow-up in `PARSER-STORAGE-FALLIBLE.md` converts the public
path's expression nodes and expression/call/patch vectors; the smallmap
follow-up converts its `global_names`. LexerError/SyntaxError allocation,
modules/imports, serialization,
traceback formatting, host caches, and global allocator policy are unchanged.

Non-Shipping diagnostics deny every converted vector stage twice, check
fixed-fatal identity and full rollback, retry, and verify live-owner allocation
headers. Separate repeated cases deny managed-value allocation, managed tracking,
and later name/kwarg vector growth after a long string/default was created; exact
heap/native accounting must return immediately without a GC. They also
exercise a denied lambda nested in an f-string and later denial while a
multi-case match owns its local patch vector, and distinguish an ordinary parser
SyntaxError. The smallmap follow-up moves the root-only production checkpoint
to the first admitted inverse-name map; host automation checks exact owner
retirement and sibling continuation. Shipping exposes neither seam.
