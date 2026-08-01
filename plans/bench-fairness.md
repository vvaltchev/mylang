# Fair C++ benchmarks: same code shape, same semantics, same flexibility

Status: PLAN (maintainer-directed, 2026-07-26). Not implemented yet.
Companion: plans/native-gap-roadmap.md (the MyLang-side levers that stay
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

(append per-bench results here as the work lands)
