# Fallible function and closure publication (#153)

`OP_LOAD_FUNCTION` now constructs a function through `ManagedHeapObjectTxn`.
The function TValue is written to the value stack only after its userdata,
closure snapshot, and recursive self binding are complete.  A failed operation
discards the unpublished object and uses the existing exact-cell allocation
terminal; it never creates a catchable Python exception.

Nested functions receive a private admitted `NameDict` snapshot.  The header,
initial four-entry table, local copies, and recursive self insertion use the
fallible exact-owner path.  The header is released if item admission fails;
later growth rebuilds before swapping.  Once `Function__ctor` has initialized
the userdata, its destructor is armed so rollback owns the declaration ref and
any assigned closure exactly once.

`FuncDecl.recursive_name` is interned when a declaration is constructed and
rebuilt while a serialized declaration is loaded.  `OP_LOAD_FUNCTION` has a
non-null precondition and uses that stored name directly: it does not call a
name-table lookup or allocation between transaction admission and commit.  The
same path covers named definitions and the first nested `<lambda>` declaration.

The old `FastLocals__to_namedict`, `NameDict__new`, `NameDict__set`,
`py_newobject`, and `py_newfunction` paths remain unchanged.  The ordinary
special-locals diagnostic remains exactly `RuntimeError: cannot create closure
from special locals` and is evaluated before admission.

Root diagnostics construct a real `Function`, call `Function__ctor`, arm its
type destructor, snapshot via `FastLocals__try_to_namedict_with`, and inject
header/item/growth failures before retrying recursive self binding.  They check
balanced declaration/closure cleanup, stack/retval/GC and exact-owner
sibling-current teardown.  The initialized host automation also executes a
recursive lexical closure and a first-use nested lambda.  A production one-shot
terminal seam is deliberately not added: source compilation would be an earlier allocation path,
so it cannot isolate the opcode without broadening compiler or serialized-code
scope.
