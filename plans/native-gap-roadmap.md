# Closing the my/cpp gap: 4.06x -> ~2.5-3x

Status: RECORDED 2026-07-26 (maintainer-reviewed direction). This is the
"what do we need" program distilled from the my/cpp >10x investigation
(the C++ disassembly audit of the worst benches). The companion effort is
plans/bench-fairness.md - some of the >10x ratios are benchmark-side
(the C++ compiler transforming the code shape); the items below are the
MyLang-side levers that remain real regardless.

## The diagnosis (one paragraph)

The >10x my/cpp cases are NOT dispatch (mostly gone) and NOT test folding
(the benches are anti-folded; the work happens). They are C++ collapsing
abstractions into registers - closures inline away, structs scalarize
(SRoA), slices become pointer pairs, linear recursion becomes a loop,
char/int reductions vectorize - while every MyLang op still runs against
48-byte memory slots with type-tag two-stores, helper calls, real
allocations (closures/slices/structs), real frames per call, and
per-element dispatch re-entry for callbacks. The program below attacks
those classes in impact order.

## The six levers (impact x feasibility order)

1. **Nativize the call protocol's fast path.** The single biggest class:
   10_recursion_deep (94x), 11_closure_counter (76x), 63_closures (53x),
   76_funcval (26x), 08/09, every call-heavy program.
   vm_frame_setup (~135 Ir) + vm_frame_leave (~92 Ir) per call; the
   COMMON shape (record reuse available, fast_bind, no coerced params) is
   ~15-20 native instructions: bump rec_n, copy the arg run, repoint the
   window, and the native ReturnV/jit_ret side already exists. The
   lean-sync work built the scaffolding - the record-reuse protocol is
   exactly what gets translated to machine code. Expected: 10 from 94x to
   ~20x, fib/closures lifted broadly.
   OPTIONAL on top: a linear-tail-recursion -> loop pass in the OPTIMIZER
   (exactly what g++ does to sumto - accumulator rotation); then
   10-class code becomes a native loop.

2. **Direct fragment calls from callback builtins.** sort/map/filter/
   make_dict/find with a native_leaf callback should `call` the fragment
   per element instead of re-entering vm_dispatch through VmInvoker
   (34_sort_custom_cmp 13x, 35_map_filter 10.5x, 12_higher_order 8.8x,
   67_make_dict). VmInvoker already owns the window + captures for the
   loop; the per-element cost to remove is the dispatch entry/exit.

3. **Borrowed read-only slices.** A loop-local slice that provably does
   not escape skips the SharedArrayObj/SharedStr allocation + COW slice
   registration and becomes base+offset+len (15/29 slice_readonly
   21x/19x, the string benches, 47_wordcount). Needs a small escape
   analysis (the slice is consumed by a read op / len / foreach within
   the statement or loop body and never stored/returned).

4. **Dyn-foreach shape specialization.** ForeachDynInit already decides
   array-vs-dict ONCE; the per-element Next still re-dispatches the
   shape and does boxed binds (66_dyn_foreach 10.5x, 74_dyn_foreach_kv
   19x). Specialize the Next per shape at Init time (two op variants or
   a stored function pointer) with unboxed binds where the element is
   scalar. 75_indexed_unpack RESOLVED (2026-07-26): the 89x was half
   bench-unfairness (reference binds vs MyLang's refcounted handle
   binds - fixed, now 36x) and half real: per row it pays the unpack
   helper + boxed binds + TWO len() calls as full CallBuiltinV builtin
   calls. NEW LEVER 4b: a `LenV` native op - len() of a proven
   str/array as an inline length read (it is among the most common
   builtin calls in loops); kills most of 75's residue and helps every
   len()-bounded loop.

4c. **Struct baked-layout nativization - DONE 2026-07-26** (see
   CLAUDE.md "STRUCT BAKED LAYOUT"). Member reads: baked offset/form in
   LoadMemberInt/Float + inline JIT fast path + tree-walker/StoreMemberV
   baked slots (slot_of scan gone from every proven hot path).
   Construction: Chunk::ctor_plans + vm_struct_ctor_planned + inline JIT
   H1 fast path (planned ctor = op_never_exits). Measured: 64 callgrind
   Ir -86.6% JIT-on / -50.8% interpreter-only; my/cpp 41.6x -> ~12x.
   src_slot extension DONE too (same day): plan fields carry the source
   SLOT (a bare-local arg reads from its own slot at ctor time, gated on
   ALL args side-effect-free; computed args in a mini-run recorded in
   the op's a-dual for visit_use_def - no chunk access needed, since
   direct srcs are locals and temp liveness only tracks temps).
   64 Ir 146M -> 137.8M. RESIDUAL to reach <=4x (N7/lever-5 territory):
   per-dst type-tag two-stores + ref-list checks (~40 Ir/iter here),
   float operand type-guard loads, float slot round-trips between ops.
   (original analysis follows) (from the 64_struct_create
   audit, 2026-07-26; C++ twin verified fair - stores + reloads through
   memory every iteration, 24 instr/iter vs OUR ~2,180). Two mechanisms,
   both compile-time-known facts resolved at RUNTIME today:
   - MEMBER READS (~79 Ir each vs 1 load): LoadMemberInt/Float ->
     jit_load_member -> vm_load_member_scalar does a LINEAR FIELD SCAN
     (`StructTypeDef::slot_of(memUid)` compares interned names over the
     fields vector) on EVERY read, then kind-dispatches - despite the
     inferencer having proven base_struct at compile time. Bake the
     field OFFSET + kind into the op (the LoadStructFieldInt treatment,
     which already byte-reads for the foreach path) and JIT-emit
     `mov rax, [bytes + off]` behind a def-identity check; the
     interpreted op should carry the pre-resolved slot too.
   - CONSTRUCTION (~390 Ir per ctor vs 2-3 stores): vm_struct_ctor walks
     the FieldDef vector generically, CALLS coerce_struct_field per
     field (validation the StructCtorV gate already proved at compile
     time), marshals a stack EvalValue buffer, then memcpys - plus the
     H1 use_count probe + StructObject refcount churn (~85 Ir/iter).
     Bake a per-field plan (offset, kind, src slot) and emit direct
     stores into the reused instance's bytes - the C++ shape.
   Expected: 64_struct_create 41.6x -> single digits; also lifts 58/65/
   77 and every struct-touching loop.

5. **Escape-analysis allocation elimination (the N7 arc proper /
   task #60).** Closures created in loops (63), struct temporaries
   (64_struct_create 56x, 77_struct_array_lit 18x), boxed values in dyn
   paths. C++ does ZERO allocations in these loops. Stack allocation /
   reuse for provably non-escaping objects; this is the value-model
   campaign's core.

6. **SIMD - the honest long-term ceiling.** 30_str_index_iterate (43x),
   46_matrix (35x), the sieve/bit benches: vectorized C++ is 2-16 lanes
   per instruction; scalar native code tops out ~2-4x behind it. A large
   separate project; NOT needed to reach the ~2.5-3x geomean target
   (the geomean is dominated by the protocol/allocation classes above),
   so explicitly deferred. (The bench-fairness plan also disables
   auto-vectorization where the comparison is meant to be shape-equal.)

## Measured anchors (2026-07-26, my/cpp)

geomean 4.06x. Worst: 10_recursion_deep 93.7x, 75_indexed_unpack 89.3x,
11_closure_counter 76.5x, 64_struct_create 56.0x, 63_closures 52.9x,
30_str_index_iterate 43.0x, 46_matrix 34.7x, 76_funcval 26.0x,
15_array_slice_readonly 21.3x, 74_dyn_foreach_kv 19.0x,
29_str_slice_readonly 18.8x, 77_struct_array_lit 17.8x,
34_sort_custom_cmp 12.8x, 35_map_filter 10.5x, 66_dyn_foreach 10.5x,
73_multi_unpack 10.2x, 06_if_branch 9.5x (check: C++ likely cmov-ifies;
a cmov tier for simple select shapes may apply).

Per-call cost anchor: 10_recursion_deep = 2.7M real calls in 0.085s =
~31ns/call (frame_setup+leave+sync overhead) vs C++'s loop-converted
~0.4ns/iteration.
