# Fair C++ benchmarks: same code shape, same semantics, same flexibility

Status: IMPLEMENTED (2026-07-26) - see the Log at the bottom; the
classification sections below are the original plan, kept for the
rationale (amendments recorded in the Log).
Companion: plans/archived/native-gap-roadmap.md (the MyLang-side levers that stay
real regardless of bench fairness).

## The principle (maintainer's rule)

We bench against **the most optimized C++ that does the exact same
thing** - not against compiler transformations that change the SHAPE of
the code. If the MyLang program performs a recursion, the C++ must
perform a recursion; if it calls a closure, the C++ must call a
closure-like value; if it iterates a dynamically-typed container, the
C++ must dispatch on a fat variant with the same flexibility. Start from
`-O3` as the baseline and disable, per bench and surgically, exactly the
transformations that are UNFAIR for that comparison. Where MyLang's
semantics are strictly richer (CoW slices, managed values), the C++ side
must either implement equivalent semantics or **embed the MyLang runtime
itself** (compile src/ code into the bench) - a managed language has
inherent overhead and the comparison must include it. Statically-typed,
fully-inferred MyLang code, by contrast, competes directly against plain
typed C++.

Every fix is **verified by disassembly** (the prove-it rule applied to
benches): after the fix, the C++ hot loop must show the same logical
shape MyLang executes - a `call` where MyLang calls, no SIMD where the
comparison is scalar, a real object where MyLang materializes one.
A fix is not done because the attribute was added; it is done when the
asm shows it.

## The toolbox (mechanisms, chosen per bench)

- `__attribute__((noinline))` - keep a call a call (closures via a
  noinline `operator()` or a noinline free function; recursion entry).
- Tail/accumulator recursion -> loop conversion: `noinline` alone does
  NOT stop it (g++ converts INSIDE the function). Per-function
  `__attribute__((optimize(...)))` with the relevant -fno-* flags
  (`-fno-optimize-sibling-calls` + verify; if g++ still
  accumulator-rotates, force the frame with a volatile local or an
  opaque()-routed recursive argument AT EACH LEVEL). VERIFY BY ASM:
  the recursive `call` must be present.
- Auto-vectorization off: `#pragma GCC optimize
  ("no-tree-vectorize","no-tree-slp-vectorize")` or the per-function
  attribute; clang fallback `#pragma clang loop vectorize(disable)
  interleave(disable)` on the hot loops. (run.py compiles with fixed
  flags; in-source pragmas keep the runner unchanged. If a bench ever
  needs real flags, add a `// BENCH-CXXFLAGS:` header comment the runner
  parses - a small runner extension, only if pragmas prove insufficient.)
- SRoA/scalarization defeat (structs must EXIST): construct through a
  noinline factory returning by pointer/into an opaque location, or
  `asm volatile("" : : "r"(&obj) : "memory")` to escape the object.
- Type-erased closures: `std::function<...>` is the honest C++
  counterpart of a MyLang FuncObject VALUE (heap-backed, indirect call).
  A raw lambda is the counterpart of an INLINED MyLang call - use
  std::function wherever MyLang performs a real closure call.
- The fat variant: a `Value` type (std::variant or a hand-rolled tagged
  union mirroring EvalValue's alternatives: none/int/float/bool/str/
  array/dict/func) with visit-based ops, for the dyn benches.
- MyLang-runtime embedding: a bench .cpp may `#include` the runtime
  sources (the types.cpp TU pattern) and drive EvalValue/SharedArrayObj/
  SharedStr directly - the exact CoW/managed semantics. CAVEAT: the
  cpp bench cache keys on (scale, sha1 of the bench source + bench.h);
  an embedded-runtime bench also depends on src/** - those benches must
  either be re-cached manually after runtime changes or the cache key
  extended to include the embedded files' hashes (small run.py change,
  do it when the first embedded bench lands).

## Per-bench classification (all 76; the work list)

**(A) Fair as-is** - typed MyLang vs typed C++, shapes already match:
01-05, 07, 08 (calls stay calls in both - verify), 13/14/16/17
(array ops), 21-28 (verify 24/26 dict loops aren't vectorized), 33,
36-45 (verify 43 sieve vectorization - see C), 47, 49-53, 55, 57-62
(verify bit benches - see C), 65, 67-72.

**(B) Compiler transformed the SHAPE - fix with noinline/attributes,
verify by asm:**
- 10_recursion_deep (94x): the recursion became a loop. Force real
  calls at each level.
- 11_closure_counter (76x), 63_closures (53x): the lambda dissolved to
  a register add. std::function (the type-erased counterpart) and/or
  noinline operator().
- 64_struct_create (56x), 77_struct_array_lit (18x): SRoA - the struct
  never exists. Escape the object / noinline factory.
- 76_funcval_dispatch (26x): verify - if g++ devirtualized the function
  pointer table, force a real indirect call (opaque index).
- 73_multi_unpack (10x), 75_indexed_unpack (89x): verify what remains
  after the dyn split (D) - the unpack itself may be legitimately
  register-resident in C++; MyLang-side profiling first (the roadmap
  flags 75 as pathological).
- 09_fib_recursive (5x): verify the C++ isn't memoizing/folding; fib's
  calls must be calls (MyLang unrolls + caches - a fair note: OUR side
  transforms here; the C++ may keep plain recursion, or we document the
  asymmetry).
- 06_if_branch (9.5x): if g++ cmov-ifies, decide: cmov is a per-op
  lowering (fair - and a cmov tier for the JIT is a roadmap idea), not
  a shape change. Likely leave C++ alone; note it.

**(C) Auto-vectorized - disable vectorization (scalar-vs-scalar):**
- 30_str_index_iterate (43x), 46_matrix (35x), 15/29 slice sums (also
  E), 43_sieve (9.3x), 54_mandelbrot, 55_float_sum, 56_sieve_bool,
  60_bit_sieve, 61_popcount, 57_bool_reduce, 18/19/20 foreach loops -
  audit each asm; disable where vectorized. (When MyLang one day
  vectorizes, revisit - keep the pragmas greppable: one marker comment
  `/* BENCH-FAIR: no-vectorize */` per site.)

**(D) Dyn twins - split static vs dynamic benches:**
- 66_dyn_foreach, 74_dyn_foreach_kv, 75_indexed_unpack, 76 (dyn
  dispatch): the current C++ twins are STATIC. Write the fat-variant
  `Value` (bench/cpp/bench_value.h) and give each a variant-based C++
  twin; the STATIC MyLang foreach benches (18/19/20/26) keep their
  plain C++ twins. Result: dyn-vs-variant and static-vs-static, both
  fair.
- 34/35/12 callback loops: maintainer verdict - MyLang's fault (the
  VmInvoker dispatch re-entry); C++ stays inlined. No bench change;
  the fix is roadmap lever 2.

**(E) Richer-semantics benches - equivalent semantics or embed the
runtime:**
- 15/29 slice_readonly: MyLang slices are CoW-registered views. C++
  twin options: (1) implement a minimal CoW slice type in bench_value.h
  (shared_ptr counted body + slice registration), or (2) embed the
  MyLang runtime and drive SharedArrayObj/SharedStr slices directly.
  Option 2 is the exact comparison; start there (it also pioneers the
  embedding mechanism for future benches).
- Future candidates: deep-clone/const benches if ever compared.

## Process (per bench)

1. Read the current C++ asm; classify (A/B/C/D/E) - record the finding
   as a comment header in the bench .cpp.
2. Apply the minimal fix; RE-READ the asm; confirm the target shape
   (call present / no SIMD / object exists / variant dispatch).
3. `bench/run.py -cl cpp --recompute --filter <bench>` to re-cache.
4. Record before/after my/cpp in this file's log section.
5. After the full pass: one suite run -> the new honest my/cpp geomean
   becomes the baseline the 2.5-3x target is measured against.

## Log

**FULL PASS LANDED (2026-07-26).** my/cpp geomean **4.06x -> 3.662x**.
All fixes asm-verified (call present / no packed arithmetic / stores per
iteration / indirect calls); all 16 changed twins re-verified to print
byte-identical results to their .my benches; the cpp cache recomputed
(same-day as the mylang measurements, so my/cpp is immune to the stale-
python-cache host-drift trap).

Class B (shape restored), before -> after my/cpp:
- 10_recursion_deep  93.7x -> 25.5x  (noinline + no-optimize-sibling-calls;
  `call sumto` back in the asm; residual = the call protocol, roadmap 1)
- 11_closure_counter 76.5x -> 30.6x  (std::function value; 3 indirect
  calls/iter in asm)
- 63_closures        52.9x -> 30.3x  (std::function + noinline factories;
  first attempt devirtualized - 0 calls - and needed the noinline)
- 64_struct_create   56.0x -> 41.6x  (asm-escape both structs; stores per
  iteration verified; the big residual is OUR ctor cost - roadmap 5)
- 76_funcval_dispatch audited FAIR as-is (1 indirect call/iter in asm)
- 09_fib_recursive audited FAIR as-is (real calls; note: MyLang's
  unroll+cache is OUR legal transform - the asymmetry is in our favor)
- 77_struct_array_lit audited FAIR as-is (operator new in the loop)

Class C (auto-vectorization disabled; packed-arith-after = 0, verified):
- 30_str_index_iterate 43.0x -> 28.7x (residual: MyLang allocates a
  1-char STRING per subscript read - a real language-side cost)
- 43_sieve 9.3x -> 7.0x; 56_sieve_bool 7.0x -> 6.4x; 57_bool_reduce
  7.2x -> 4.2x; 14_array_subscript 2.2x -> 2.0x; 18/19/20 foreach
  ~unchanged (their vector uses were setup, not the hot loop).
- 46_matrix_mult RECLASSIFIED: not vectorized (asm-audited) - its 33.3x
  is a GENUINE MyLang gap (boxed array-of-arrays vs flat doubles).
- 54_mandelbrot / 55_float_sum: not vectorized (audited) - left alone.

Class D (fat-variant twins, bench_value.h):
- 66_dyn_foreach   10.5x -> 5.7x  (Value array + dyn accumulator +
  per-entry shape dispatch)
- 74_dyn_foreach_kv 19.0x -> 7.4x (Value-keyed dict, Value k/v arith)
- 75_indexed_unpack: RE-AUDITED after the 88.9x was challenged. The C++
  loop was structurally honest (asm: 8 scalar instr/row, re-run every
  rep, no SIMD/hoisting) but `const auto &row` binds by REFERENCE -
  which MyLang's unpack semantics cannot do. Class-E fix: per-row
  refcounted HANDLE binds for name/val (volatile refcnt so the balanced
  ++/-- pair isn't folded - MyLang's are real RMWs; 30 refcnt load/store
  sites verified in main) + the STRICT arity check. 88.9x -> 36.0x
  (the C++ 0.003 -> 0.007s - the managed-bind semantics were ~half the
  story). The remaining 36x is OURS: per row, an unpack helper call +
  boxed binds + TWO len() calls running as full CallBuiltinV builtin
  calls where C++ reads .size() inline - a `LenV` native op (inline
  length read for a proven str/array) is the obvious lever, added to
  the roadmap.

Class E (managed-slice twins; DEVIATION from the plan: embedding the
runtime was rejected after scoping - types.cpp's extern graph drags the
whole 22-TU interpreter into a single-file bench (plus a second main) -
the twins instead MIRROR the exact mechanics: refcount + slices-set
registration per array slice (SharedArrayObj's design; the set insert IS
in the asm), refcounted view + fresh 1-char string per char read for
strings):
- 15_array_slice_readonly 21.3x -> 8.6x
- 29_str_slice_readonly   18.8x -> 19.6x (~unchanged: the twin's 1-char
  strings are SSO-cheap in C++; MyLang's SharedStr-per-char is the real
  cost - a language-side item, pairs with the 30 residual)

NEW HONEST WORST LIST (the roadmap's targets): 75 36.0x (len()-as-
builtin-call + unpack helper; see its entry),
64 41.6x, 46 33.3x, 63 30.3x, 11 30.6x, 30 28.7x, 10 25.5x, 76 24.7x,
77 17.2x, 73 9.9x, 68 7.8x, 60 7.5x, 74 7.4x.


## ⛔ THE 2026-07-26 CALLBACK VERDICT IS REVERSED (maintainer, 2026-08-14)

The class-D list above records:

> **34/35/12 callback loops**: maintainer verdict - MyLang's fault (the
> VmInvoker dispatch re-entry); C++ stays inlined. No bench change; the
> fix is roadmap lever 2.

**That verdict no longer stands**, and the maintainer stated the
principle more precisely than "callbacks":

> C++ inlining is FAIR, but not when it's about STL function calls while
> we're using MyLang BUILTINS on the other side. MyLang callbacks cannot
> be inlined into the hard-coded builtins, while the STL doesn't have
> this problem.

So the test is NOT "does the C++ inline a lambda". It is **"is the
MyLang side a hard-coded BUILTIN that structurally CANNOT inline its
callback?"** A `std::sort` that inlines its comparator is competing
against `builtin_sort_lv`, which reaches its comparator only through
VmInvoker - the C++ enjoys an inlining the MyLang side cannot have at
any optimization level.

**Where the line falls, and it matters:** `12_higher_order` is
`func apply(f, x) => f(x)` - a USER function, which MyLang's own
inliner, devirtualizer and bytecode splice can all collapse. g++
inlining `apply` (asm: zero calls, the loop folds to `imul`) is
therefore FAIR and 12 is UNCHANGED. Only the builtin-plus-callback
shapes were touched.

THE COMPLETE AUDIT - every callback passed to a builtin in bench/my,
which is 5 sites across 4 benches:

    34_sort_custom_cmp   sort(a, func(p,q) => p<q)
    35_map_filter        map(func(x) => x*2, a)
                         filter(func(x) => x%3==0, b)
    57_bool_reduce       make_array(N, func(i) => i%2==0)
    67_make_dict         make_dict(ks, func[r](k) => k*k+r)

All four twins now call a `std::function` through a `noinline` helper
that stands in for the builtin. ASM-VERIFIED per the prove-it rule -
indirect calls present in each hot loop where there were none:

    34_sort_custom_cmp   14 indirect calls in the emitted asm
    35_map_filter         6
    67_make_dict          3
    57_bool_reduce        3
    12_higher_order       0  (unchanged, and correctly so)

The builtin uses with NO callback stay as they are, and for the reason
the rule gives: `sort(a)` without a comparator, `sum`, `min`, `max` and
`find` are hard-coded C++ on BOTH sides, so an STL twin is the honest
counterpart (33_sort_ints, 36_sum_builtin, 38_min_max, 39_find_builtin,
52_cse_dedup).

**WHY THE FIX GOES ON THE BENCH SIDE AND NOT IN THE IMPLEMENTATION**
(maintainer, 2026-08-14): to give MyLang the inlining `std::sort` gets,
we would have to **emit the sorting function itself** - JIT-generate a
sort specialized to this comparator, per call site - *"which is overkill
for MyLang"*. The asymmetry is structural and we are choosing to keep
it, so the honest move is to stop the C++ side from exploiting an
advantage the design will never have.

NOTE WHAT THIS DOES **NOT** RULE OUT. "Emit the sort" is overkill; a
**typed callback ENTRY** is not the same thing. The comparator would
still be called once per comparison - no inlining, no emitted
algorithm - but it could be entered with two raw `int_type`s instead of
two boxed `EvalValue`s bound through the generic parameter path. On
34_sort_custom_cmp that is ~72 Ir of boxing in `sort_core` plus most of
the ~152 Ir inside `VmInvoker::invoke`, out of ~300 per comparison.
That optimization stays on the table; the inlining does not.
