# #153 fallible string-buffer slice

This is a bounded caller-family conversion, not completion of the hard memory
cap. Production `PK_MALLOC` / `PK_REALLOC` remain non-denying for every legacy
caller. No exception, `longjmp`, VM reset, creator-visible hook, or larger limit
is introduced.

## Converted contract

`c11_sbuf__try_ctor`, fallible char/C-string/float/pointer/quoted/padded writers,
and `c11_sbuf__try_py_submit` use `MantlePocketPy_TryAdmittedVectorReserve` and the
existing checked append plan. Quota denial and libc allocation failure leave
the old vector pointer, length, capacity, and bytes unchanged. Quoted, float and
pointer output is fully materialized or sized before one append, so each writer
is failure-atomic. Submit appends its NUL before transferring ownership and
never publishes the backing string on failure. Cleanup is the allocation-free
vector destructor. Size and metadata failures record the same exact-owner fixed
fatal channel; admitted reserve already records quota and backing failures.

The padding writer accepts its count while still `int64_t`, validates exact
payload length, plans/reserves once, then commits sign/source/padding bytes and
logical length together. `str.zfill`, `str.ljust`, and `str.rjust`
(`upstream/pocketpy.c:8703,8726`) now use it. Their prior no-op identity behavior,
UTF-8 zfill width, sign placement and one-byte fill-character validation remain
unchanged. Huge widths fail before narrowing, iteration, or result publication.

`str.join` (`upstream/pocketpy.c:8684`) now owns one explicit iterator stack
root after `py_iter` succeeds. Its admitted ctor, separator/item writes and try
publication restore that root and destroy any still-attached buffer on every
false return. Nested `py_next` exceptions remain ordinary Python exceptions;
allocation denial remains exception-free. Exhaustion pops the iterator before
fallible managed publication, whose failure preserves the current retval and
frees the detached sbuf storage. Embedded NULs remain counted payload bytes.

The converted non-recursive native representation family is now exactly:
`float__repr__`, `object__repr__`, `type__repr__`, `str__repr__`,
`bytes__repr__`, `function__repr__`, `Enum__str__`, and `module__repr__`
(`upstream/pocketpy.c:7562,7972,8010,8462,8986,24318,25204,32783`). Each uses
only try operations, destroys an
unsubmitted buffer on false, constructs no Python exception, and returns false
into the generation-local `RES_TERMINAL` path. A creator `try/except` therefore
cannot catch a buffer denial and continue. The managed-string follow-up changes
only their shared final `c11_sbuf__try_py_submit`: it calls `py_trynewstrv`,
always frees detached buffer storage on denial, and returns through the same
terminal. Legacy sbuf submitters remain on `py_newstrv`; global allocator policy
remains non-denying.

Non-Shipping deterministic coverage has two parts. The C test
`py_mantle_sbuf_storage_test` uses only fixed 512-byte arrays and an explicit
per-call realloc callback to prove denied quoted growth and denied submit keep
the pointer/length/capacity/bytes and output unchanged, retry can move while
preserving bytes, an independent sibling stays unchanged, and float/pointer
formatting produces the expected complete value. The initialized automation
temporarily gives one live generation exactly enough quota for the tiny initial
buffer, then calls the real `bytes.__repr__` leaf with a modest 100-byte source.
A second exact-generation case calls real sign-aware zfill with width 100 through
a nested callable. Both growth requests are denied before libc; terminal retires
only the exact generation, code after the call and its `except` block do not run,
and a sibling continues. Fixed-array padding diagnostics reject a synthetic
`INT64_MAX` count before the reserve callback, then denial/retry verifies the
original buffer fields/bytes and complete ordering. The initialized test also
checks ordinary output and no-op identity for the converted family. No large
allocation or actual OOM is attempted.

The join root diagnostic uses two modest long items and a long separator to
cross both sbuf growth and the managed-string threshold, including embedded
NULs. It checks an ordinary non-string-item `TypeError`, then clamps the exact
owner quota immediately before buffer construction, first growth, and managed
publication. Each denial preserves the checkpoint retval, restores the caller
stack/sibling sentinel, leaves no reservation, and reports the exact owner
generation. An initialized terminal case proves creator `try/except` cannot
continue and a sibling remains healthy.

The detached temporary string follow-up converts `c11_sv__replace2` and
`c11_sv__u8_slice` to explicit-result `c11_sv__try_replace2` and
`c11_sv__try_u8_slice`. Both preflight the exact byte count with checked
arithmetic, make one admitted exact-size allocation, fill only private storage,
and publish the output pointer last. The immediate public callers `str.replace`
and the slice branch of `str.__getitem__` use `py_trynewstrv` and release the
detached result after either successful or denied managed publication. Thus a
helper denial leaves the helper output and Python retval unchanged, while final
publication denial cannot leak or expose the temporary. Explicit byte spans
preserve embedded NULs. UTF-8 slice iteration retains normalized positive,
negative and stepped behavior. A bounds-checked span walker groups complete
lead/continuation runs and treats stray continuations, invalid leads, and
truncated runs as individual bytes. It therefore covers every byte admitted by
`bytes.decode` without reading beyond the explicit span. Empty-pattern replace
and stepped slicing each use linear passes over these spans; widened stride
arithmetic keeps an extreme negative step defined. Empty-pattern replacement
inserts at those deterministic boundaries rather than entering the legacy
zero-progress loop.

Its bounded root diagnostic uses fixed stack arrays and an injected one-call
malloc failure to verify unchanged helper output, rolled-back reservation,
exact backing-failure identity and sibling/stack/retval preservation. A modest
639-byte replace result then forces the managed large-object publication path
under a zero-headroom quota, verifies unchanged retval and detached-storage
cleanup, and confirms the same sibling remains healthy. Initialized automation
also covers exact-generation noncatchable terminal retirement, ordinary
zero-step `ValueError`, embedded NUL replace, empty-pattern replace, and UTF-8
forward/reverse stepped slices. Fixed malformed spans cover continuation-only,
truncated, and invalid-lead bytes in both replacement and reverse slicing. A
384-byte valid multibyte span exercises the structural linear paths without a
timing assertion. No actual OOM or large synthetic request is used.

The split follow-up converts the low-level public-string helper set to
`c11_sv__try_split`, `c11_sv__try_split2`,
`c11_sv__try_splitwhitespace`, and `c11_sv__try_splitlines`. Each validates its
explicit span, computes an exact result count, makes one admitted exact vector
reserve, fills only private storage, and publishes the vector last. Byte
spans preserve embedded NULs. Whitespace classification uses unsigned bytes;
the splitlines walker recognizes the existing ASCII, CRLF, NEL, LS and PS
boundaries with explicit remaining-length checks and advances every other
valid or malformed byte sequence through the bounded span walker.

`str.split` and `str.splitlines` retain that private span plan until one
pre-sized admitted `py_TValue` vector has been filled with fallible managed strings. Only
then is an admitted list object constructed and the completed vector and retval
published. Helper, list-backing, element-string, or list-object denial destroys
all still-native vectors, leaves retval and caller stack unchanged, creates no
Python exception, and returns through the exact-generation terminal.
Already-created managed strings on a later denial remain heap-owned but
unreachable; terminal retirement reclaims that generation without a dangling
native allocation. No Python/user dispatch occurs during construction.

The vendored API has no `maxsplit` parameter. This slice preserves its current
`argc > 2` ordinary `TypeError` rather than adding one. It also preserves
whitespace empty-field filtering, explicit-separator empty fields,
trailing-line behavior and `keepends`. Bounded diagnostics use a 1203-byte input,
an embedded-NUL separator, malformed/truncated line bytes and real quota clamps
at helper and list-backing stages. A second real clamp is armed only after one
600-byte managed element succeeds, then denies the next element and proves the
first remains off-root but heap-owned until an explicit collection releases its
deferred charge. A distinct test-only caller-boundary seam records the same
exact-owner instance-limit terminal immediately before list-object allocation;
it routes through the production failure cleanup for a fully populated private
item vector. This is not claimed as managed-heap allocator injection: the
allocator's arena/backing denial is covered separately by
`py_mantle_managed_heap_storage_test`. Both new stages verify unchanged retval,
stack and sibling, native vector cleanup, zero reservation, exact fatal identity
and post-collection charge rollback. Each deferred stage first warms the
collector's retained root-vector capacity, then also checks the admitted-large
object count rises by exactly one or two and returns to its baseline. This keeps
collector infrastructure growth distinct from leaked split storage.
Initialized automation still proves a
noncatchable terminal and healthy sibling. No actual OOM or large allocation is
attempted.

This walker is deliberately local to empty-pattern replacement and the string
slice branch, whose `pk__parse_int_slice` length now comes from the same span
count. The shared legacy `c11_sv__u8_length` / `c11_sv__u8_getitem` pair is not
changed: integer indexing, strip, and other immediate callers would require a
separate malformed-string semantic audit. In particular, integer indexing of
arbitrary malformed `bytes.decode` output remains outside this slice.

## Immediate caller ledger

The following compiled/enabled immediate caller families remain on legacy
`c11_sbuf__ctor` / write / submit wrappers and are **not converted** by this
slice (line locations are from this patched amalgamation and are navigation
hints):

- Core/native formatting still unconverted: `str__mod__` and `str_format`
  (`:8304,8965`). They invoke nested/user dispatch and require their own
  reference-lifetime and rollback audit.
- Other plain string helpers and compilation: compiler context paths and
  `c11_string__new3` (`:11143-16649`). SourceData and public lexer literal
  assembly are converted by their later dedicated transactions.
  `c11_string__new3` remains legacy because all six
  enabled immediate callers are import filename/cache construction, an
  explicitly excluded boundary for this slice. These include void constructors
  and ownership transfers that require separate rollback.
- VM/error/debug reporting: formatted-string bytecode, `pk_format_object`,
  managed-heap debug callback, line-profiler/pool summaries, `pk_print_stack`,
  `disassemble`, BaseException repr/str, `formatexc_internal`, and
  `py_exception` (`:18778-21725`, `:24612-24729`, `:34075-34283`). Error and
  traceback construction cannot be used to report allocation denial.
- Builtins and compiled modules: array2d renderers, `input`, `hex`, `bin`,
  `print`, `Enum__repr__`, JSON dumps, pickle submit, memory diagnostics, and
  color helpers (`:22112-30820`). Array2d and pickle are import-restricted in
  Mantle but remain compiled and therefore stay in the ledger. `Enum__repr__`
  performs nested `str`/`repr` dispatch and is deliberately not grouped with
  the now-converted non-recursive `Enum__str__`.
- Runtime object and host-facing helpers: dict/list/slice/tuple repr, relative
  import path assembly, package attribute path assembly, and `py_newfstr`
  (`:32853-36070`). Recursive container repr remains a separate family.

The value-returning legacy `c11_sv__split` remains only for the compiled package
component parser in relative-import handling (`upstream/pocketpy.c:34244`). That
caller owns an aborting vector and crosses excluded import/module state, so it is
not silently routed through the converted public-string contract. The old
value-returning `split2`, `splitwhitespace`, and `splitlines` helpers have no
remaining enabled callers and are removed.

The DAP/socket block around `:17690-18022` is lexically present but excluded by
Mantle's `PK_ENABLE_OS=0`; enabling OS support must reopen it. Direct
`c11_string__new` / `new2` allocation and their SourceData, lexer, FuncDecl,
module metadata, import/file, and traceback-name callers also remain legacy.
The managed Python string allocation reached after legacy
`c11_sbuf__py_submit` remains unconverted. Only the explicit try submit above is
admitted. These gaps are why #153 remains open.
The fixed fatal channel also retains its existing first-record-per-VM contract;
queueing a second simultaneous generation fault while a sibling record is still
pending is a broader terminal-channel problem, not solved or claimed here.
