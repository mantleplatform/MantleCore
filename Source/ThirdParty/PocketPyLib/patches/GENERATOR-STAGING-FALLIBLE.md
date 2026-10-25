# Generator staging and continuation allocation (#153 slice)

Only Python generator-function creation and resume are converted here. Normal
and simple function calls, public object/list APIs, the global allocator policy,
and arbitrary runtime constructors keep their legacy behavior.

Generator `*args` and `**kwargs` are prepared with admitted unpublished object
transactions. They remain rooted until the complete initial continuation is
owned by a generator. Generator frames use an exact-owner admitted allocation
instead of the VM-wide fixed frame pool, so a suspended task cannot retain an
uncharged or inactive-owner overflow allocation. The generator object and its
continuation list are separately rooted transactions; the return value is
published only after the frame, list backing, snapshot, object slots and
destructors are valid.

Resume checks fixed-stack room before restoring a continuation. On yield it
preflights admitted list growth and copies the complete replacement snapshot
before committing list length or detaching the frame from the VM. Denial pops
and deletes the still-owned frame, publishes no yield, and returns through the
existing allocation-independent exact-generation terminal. `Frame__begin_try`
similarly reserves admitted exception-frame backing, initializes the new entry,
and commits its length last.

Non-Shipping diagnostics provide deterministic generator-object, initial-list,
frame, yield-snapshot-growth, and exception-frame-growth denial sites. Host
automation repeats terminal denial, retries through real `task.spawn` and
`ResumeTask`, forces collection after rollback, checks exact owner retirement,
same-ID replacement, task removal, fatal drain, absence of Python exceptions,
and sibling continuity.

This is not completion of #153. The GC root-work vector still uses the legacy
aborting growth path. StopIteration construction at normal generator return and
ordinary exception/traceback creation also remain legacy allocation paths.
Those are explicit blockers to claiming an end-to-end hard cap; broad global
constructor or allocator changes are intentionally excluded from this slice.
