# Fallible compiler class publication (#153)

`OP_BEGIN_CLASS` is now the only transaction boundary for a compiler-created
class.  Before changing the operand stack or exposing the type, it prepares any
replacement `VM::types` backing and constructs a complete admitted type object,
including its initial `NameDict`.  The unpublished managed object is transaction
pinned.  Commit appends the complete `TypePointer`, establishes the permanent VM
root, clears the pin, and replaces the base operand without allocation or a
callback.  A denied preflight leaves the table, stack, retval, output and heap
accounting unchanged and reaches the existing allocation-independent,
exact-generation terminal.

The type table never reallocates backing through an inactive owner.  Growth is
an allocate/copy/swap operation under the current producer; the old backing is
freed only after the new table and class are published.  Admitted pool blocks
likewise reuse an arena only when its immutable header names the same exact
quota generation.  Retained class objects remain VM roots, and their object and
attribute-table headers continue charging their original exact ledgers until VM
reset.

Reload preserves the existing class object and type index.  It first allocates
an empty replacement `NameDict` under the active producer.  Only after that
preflight succeeds does an allocation-free step swap the backing, reinitialize
the existing `py_TypeInfo`, and refresh its existing `TypePointer`.  The old
backing is then freed through its own immutable header.  Denial leaves the old
identity, attributes, type-table entry and accounting untouched.  Once reload
BEGIN succeeds, a later body failure retains the same reinitialized object and
any partial new body state.

After BEGIN returns there is deliberately no class rollback.  Class-body,
decorator, `on_end_subclass`, final module binding, and post-binding validation
errors follow the existing END ordering and unwind, while the published class
remains rooted and valid.  Class attribute insertion and module-backed final
binding use the admitted `NameDict` seam, so hard-cap denial is terminal without
freeing or tombstoning the exposed type.  This ticket provides hard-cap memory
safety; it does not promise rollback of arbitrary Python-visible side effects.

Root diagnostics repeat forced type-table, initial-attribute-table, and forced
fresh-pool-arena object denial.  The last bypasses same-owner arena reuse and
therefore proves provisional-object rollback as well as exact fatal ownership
and unchanged table/output/SP/retval/accounting.  The successful fixture is
module-bound and reused by later diagnostic calls through reload, so repeated
runs do not grow `VM::types` or retain another type/NameDict charge.
Diagnostics also exercise collision-driven attribute growth denial and retry,
and check reload denial/success identity.  Initialized automation retains a
decorated class and instance across an ordinary decorator error and GC, checks
method dispatch and `isinstance`, verifies successful and body-failing reloads
preserve identity, forces a real `OP_BEGIN_CLASS` terminal plus a
post-exposure final-binding terminal, and uses a diagnostic `on_end_subclass`
base which retains both the class and an instance before an exact terminal.
All terminal paths confirm sibling continuity.

Legacy `pk_newtype`, `pk_newtypewithmode`, public dynamic attribute APIs,
native/bootstrap type registration, imports, deserialization and global quota
policy remain unchanged.
