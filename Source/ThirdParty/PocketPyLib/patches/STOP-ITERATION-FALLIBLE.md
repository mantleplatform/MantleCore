# Exact StopIteration construction (#153 slice)

The legacy `StopIteration()` path called the generic Python constructor. That
could allocate after an iterator had already reached exhaustion, and normal
generator return additionally stored terminal state before the allocation.

This slice adds one private transaction for the exact built-in
`tp_StopIteration` type. It uses `ManagedHeapObjectTxn__try_new_with`, directly
initializes `BaseException.args`, `inner_exc`, and `stacktrace`, arms the base
exception destructor, commits, and then calls `py_raise`. Supplying a normal
generator return value copies the existing `py_TValue` into `args`; it allocates
neither a call argument tuple nor a second temporary exception object.

All built-in exhaustion leaves retain their ordinary `return StopIteration()`
spelling, so list/tuple, range, str/bytes, array2d, and dictionary iterators
now share this exact path. `py_next` retains its 1/0/-1 contract: successful
StopIteration is caught there and preserved in `last_retval` exactly as before.

ROOT-only diagnostics force a fresh admitted arena only while an armed exact
StopIteration transaction executes, then deny its actual malloc. A reusable
small-object arena therefore cannot bypass constructor denial, and the force
mode cannot leak into later resume or unwind work.
The bounded native check exercises exhausted list and range iterators through
`py_next == 0`, verifies the normal-return value is retained in the exact
exception payload, then repeats a denied list exhaustion and consumes the fatal
to compare VM, instance, generation, and allocation-failed reason. The
generator sandbox automation repeats a normal-return denial, verifies there is
no Python exception or stale fatal, confirms owner/task retirement and sibling
continuity, carries `153` through `yield from`, and runs a fresh generation
through ordinary exhaustion.

On normal generator return the interpreter has already consumed and deleted the
frame before the exception can be built. A denial is an exact noncatchable
terminal, not a recoverable `next()` failure. The generator is consequently
made inert (`state == 2`) on both success and denial: retaining `state == 1`
after a denied allocation would expose a null frame to an invalid direct retry.
This is the bounded terminal-only exception to the usual publication ordering;
the raw normal-return value is also cleared from `last_retval`. There is no
Python exception, no reusable partial result retaining the retired generation,
and the first allocation fatal remains the owner of the unwind.

This does not generalize exception construction and does not address arbitrary
Python iterator methods, exception traceback/formatting, GC root work, module
or import paths, caching, or production denial controls.
