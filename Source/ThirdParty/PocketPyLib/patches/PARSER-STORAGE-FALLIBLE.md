# Parser-owned storage transaction (#153)

## Converted ownership

The admitted public `py_compile` / `py_exec` / `py_eval` transaction now covers
the parser's directly owned expression nodes, exact-size sequence child arrays,
context expression stacks, positional and keyword call vectors, chained-
comparison jump vectors, and match-case patch vectors. Native-signature
compilation continues to pass the same non-admitted compiler transaction and
therefore retains the legacy aborting constructors and vector operations.

Every admitted node allocation and vector growth records the fixed fatal for
the current quota-cell generation and latches the existing compiler terminal.
No failed append publishes a length or element. A constructor that has already
accepted child ownership destroys those children when its own node allocation
is denied; sequence children remain on the context stack until both the node
and exact child array exist. Failed expression-stack publication destroys the
unpublished tree, while failed call publication destroys only the just-popped
argument and leaves the call tree owned by the stack. Temporary comparison and
match vectors are destroyed before the compiler transaction unwinds.

The enclosing compiler rollback still destroys contexts, expression trees,
tokens, managed constants and the unpublished CodeObject exactly once. Public
output, retval, VM stack, committed/reserved accounting and generation
accounting remain unchanged. The fixed allocation terminal wins over any later
parser diagnostic; ordinary grammar failures still return `SyntaxError`.

## Diagnostics

Non-Shipping root diagnostics deny all twelve reachable storage stages twice:
expression object, sequence child array, and initial plus growth allocation for
the expression stack, positional call, keyword call, comparison jump, and match
patch vectors. Each stage retries through the real public compiler
implementation and checks unchanged output, retval, stack, sibling root,
quota/VM accounting, exact fatal identity, and full cleanup. A separate
malformed call verifies ordinary `SyntaxError` remains distinct. Fixture-only
identifier names are interned before the accounting baseline because the
process-global name table intentionally outlives a compiler transaction; this
keeps the accounting assertion scoped to parser-owned storage without reducing
the unique-keyword growth case.

A root-only one-shot checkpoint denies the first expression node reached by a
real `py_exec`. Focused host automation verifies noncatchable terminal return,
retirement of only the exact owner, fatal drain, and continued execution in the
same sibling generation. Shipping exposes neither diagnostic seam.

## Remaining boundary

The later bounded follow-up in `COMPILER-SMALLMAP-FALLIBLE.md` converts the
admitted path's `global_names`; import/package buffers and ordinary
`SyntaxError` storage remain unchanged. Module/bootstrap/require/import/cache
publication, generic runtime containers and objects, PushValue/host conversion,
traceback/error formatting, serialization/deserialization, and the global
allocator policy remain unchanged. Deserialization does not enter the parser.
