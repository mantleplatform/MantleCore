# Compiler smallmap allocation contract (#153)

This bounded follow-up covers only the admitted public in-memory compiler path's
`Ctx.global_names`, `CodeObject.varnames_inv`, `CodeObject.names_inv`, and
`FuncDecl.kw_to_index` maps. The generic `c11_smallmap` constructor and setter
remain infallible for every other caller.

Each admitted map initializes its backing vector through `CompilerTxn` and uses
a compiler-local sorted-insert plan. Initial capacity and later growth are
separate diagnostic stages. Planning checks the append geometry and completes
any admitted reserve before moving or publishing entries. A denial latches the
existing exact-generation fixed allocation fatal; it does not construct or
replace a Python exception.

`CodeObject` varname/name insertion plans and reserves both the ordered name
vector and its inverse map before publishing either length. `FuncDecl` keyword
insertion similarly reserves the keyword map and default-argument vector before
publishing their entries. A failed reserve can change only unpublished capacity;
the enclosing compiler transaction destroys that storage and restores native
accounting. Successful `CodeObject` graphs retire their weak transaction links
before publication exactly as before.

Non-Shipping diagnostics deny all eight initial/growth stages twice, retry each
source, validate sorted vector/map agreement and exact header ownership for
persistent maps, and verify temporary global-map debit/free accounting. They
also preserve the output object, retval, VM stack, rooted sibling value and
ordinary `SyntaxError` distinction. The existing initialized host regression
now includes these diagnostics, and its production denial enters through the
first admitted inverse-name map so exact-owner retirement and sibling-generation
continuation exercise this slice.

Excluded paths are unchanged: native-signature compilation, debugger and
pickle/CodeObject deserialization, serialization, imports/package buffers, the
context string-dedup map, modules/bootstrap/cache, and every other legacy
smallmap. No global allocator or `c11_smallmap` policy is changed.
