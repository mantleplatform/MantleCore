Vendoring quickjs-ng v0.16.2 for UE 5.8 / MSVC 14.44 itself needed no patch:
warnings that upstream's own code trips under UE's default warning level are
relaxed at the module level in QuickJSLib.Build.cs instead of by editing
upstream/ in place.

The creator-script sandbox did need source changes. UnrealBuildTool compiles
upstream/ as-is and applies nothing from this folder, so each patch below is
ALREADY APPLIED in upstream/; the .patch file is the replayable record of it.
Every changed hunk in upstream/ carries a "MANTLE" comment, so
`grep -n MANTLE upstream/quickjs.c upstream/quickjs.h upstream/libregexp.c`
lists all of them.
Never change upstream/ without adding or updating a patch here.

To re-vendor: copy the new upstream tag over upstream/ (see ../VERSION), then
replay each patch in order from the mantle repository root. This folder is the home;
the MantleCore plugin's Source/ThirdParty/QuickJSLib is a copy, brought up to date
afterwards with python tools/copies/check_copies.py --sync:

    git apply --directory=client/Source/ThirdParty/MantleGameQuickJSLib/upstream \
        client/Source/ThirdParty/MantleGameQuickJSLib/patches/<name>.patch

If a hunk no longer applies, re-read what it guards (below) against the new
upstream code rather than forcing it.

0001-mantle-386-deny-string-codegen-and-poll-array-loops.patch   (#386)

  1. JS_MantleDenyStringCodeGeneration(ctx) (quickjs.h, quickjs.c): a
     one-way, per-context switch checked in JS_EvalObject, the single funnel
     for every string-to-code request running JS can make -- direct eval,
     indirect eval, and js_function_constructor, which backs Function,
     GeneratorFunction, AsyncFunction and AsyncGeneratorFunction. Deleting
     global names cannot close this: the generator/async constructors are
     reachable from any function literal via
     Object.getPrototypeOf(...).constructor. The embedder's own JS_Eval /
     JS_Eval2 calls (module compile, static imports, preludes) go through
     JS_EvalThis2 -> JS_EvalInternal, never JS_EvalObject, so they keep
     working in a denied context. quickjs-ng has no host hook for this
     (no HostEnsureCanCompileStrings equivalent), and JS_AddIntrinsicEval
     cannot be left out because it also installs the parser the embedder's
     JS_Eval needs. Upstream would want a general hook instead of this
     Mantle-named switch.

  2. js_poll_interrupts() at the top of every Array.prototype loop that walks
     a caller-controlled array-like length and may do no allocation and no
     call back into JS per iteration (holes, or writes dropped by a typed
     array on the prototype chain), so the interrupt handler -- the only CPU
     limit on creator scripts -- still runs: join (and toString /
     toLocaleString through it), concat, fill, reverse, slice and splice,
     flat / flatMap, sort (collection and trailing delete), and
     JS_CopySubArray (shift, unshift, splice, copyWithin). This is the same
     one-poll-per-element pattern upstream already uses in every, some,
     forEach, map, filter, reduce, find, includes, indexOf and lastIndexOf
     (for the last three, upstream polls only in their generic branch; 0002
     adds the fast-array branch), and could reasonably go upstream.
     Deliberately left alone: loops that allocate proportional memory before
     or while iterating (with, toReversed, toSpliced, toSorted, Array.from,
     String.prototype.repeat / padStart / padEnd, typed-array methods, whose
     length is backed by an allocated buffer) are bounded by the runtime
     memory limit; loops that call JS per element already poll inside
     JS_CallInternal.

0002-mantle-492-charge-unpolled-native-scans.patch   (#492)

  Native scans reachable from creator scripts did linear or quadratic work
  inside one call while reaching the interrupt handler rarely or never, so
  one call -- or a JS loop repeating a linear one, which polls only once per
  call -- could burn server CPU far past the instruction budget. Polling once
  per outer iteration is not enough (1e5 keys = 1e5 polls = 10 handler calls
  for 5e9 comparisons), so the scans themselves are charged: one poll per
  element or comparison, the count the equivalent hand-written JS loop pays.

  1. js_mantle_charge_interrupts(ctx, units): exactly equivalent to calling
     js_poll_interrupts() 'units' times, without a per-unit branch, for scans
     too tight to poll per step.

  2. js_proxy_get_own_property_names (the Proxy ownKeys trap result, behind
     Reflect.ownKeys, Object.keys, for-in and friends): one poll per element
     of the trap's returned array-like; the O(len^2) duplicate check charges
     i per key before find_prop_key(tab, i); the non-configurable /
     non-extensible target check charges len per target key before
     find_prop_key(tab, len).

  3. JS_JSONStringify with a replacer array: one poll per replacer element
     (a hole-only array of length 2^32-1 is otherwise a long unpolled loop).
     Its O(n^2) de-duplication goes through js_array_includes(), item 4.

  4. Array.prototype.includes / indexOf / lastIndexOf: one poll per element
     in the fast-array branch too, as upstream already does in the generic
     branch. This also covers JSON.stringify's replacer de-duplication and
     its cycle check on the nesting stack.

  5. String.prototype.indexOf / lastIndexOf / includes / startsWith /
     endsWith: the naive search compares the needle at every position, so a
     needle that keeps almost matching ('a'.repeat(1e6) searched for
     'a'.repeat(5e5) + 'b') is O(len * needle) in one call. Each position is
     charged before its comparison: the needle length if its first character
     matches (string_cmp() may walk the whole needle), otherwise 1.

  6. string_indexof(), behind String.prototype.split, replace and replaceAll
     with a string pattern: now takes ctx, charges each character scan plus
     the needle length before each candidate comparison, and returns -2 with
     the interrupt pending; both callers turn -2 into their exception path.

  7. %TypedArray%.prototype.indexOf / lastIndexOf / includes: the scanned
     element count is charged once after the scan (k ends where the scan
     stopped; the memchr branch now leaves k there too). After the fact on
     purpose, so memchr and the typed loops stay tight: one scan is bounded
     by the buffer, which the memory limit bounds; the repeat-from-JS case
     is what needed the charge.

  8. The interrupt period: JS_INTERRUPT_COUNTER_INIT (quickjs.c) and
     libregexp.c's INTERRUPT_COUNTER_INIT go from 10000 to 1000 polls,
     published as JS_MANTLE_INTERRUPT_COUNTER_INIT in quickjs.h together
     with JS_MANTLE_INTERRUPTS_PER_BUDGET_UNIT (10). Other linear natives
     that do proportional work per call without searching (copies, fills,
     joins, string comparisons and conversions) are deliberately not charged
     per element -- there are too many to patch one by one. Mantle's handler
     (MantleQuickJS.cpp) instead also checks the wall clock and stops a call
     once its CPU charge exceeds the script's rolling-budget headroom, and
     that check is only as prompt as the handler is frequent: a loop of 4 MiB
     typed-array copies costs ~0.5 ms per ~3 polls, so at 10000 polls the
     stop could land seconds past the deadline; at 1000 it lands within
     ~0.2 s. The cost is one handler call (a counter bump, a compare and a
     clock read, ~20-30 ns) per 1000 polls, under 1% even for the tightest
     bytecode loop; 100 would tighten the bound another 10x for several
     percent on every script, which the per-element charges above make
     unnecessary for searches. Every handler that counts calls against a
     budget (MantleQuickJS.cpp, and the TypeScript compiler budget in
     MantleQuickJSRuntime.cpp) multiplies its limit by
     JS_MANTLE_INTERRUPTS_PER_BUDGET_UNIT, so each budget still means the
     same number of polls.

  Results and error messages are unchanged; only the interrupt handler is
  reached sooner. Deliberately left alone: the algorithms themselves (a
  hash-based de-duplication or a linear-time string search would be a
  larger divergence from upstream).
