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

1. **Nativize the call protocol's fast path.** STEP 1+2 LANDED
   2026-07-26 (the profile split + the lean C++ push/leave; see
   CLAUDE.md "LEVER 1"): the measured per-call protocol was ~500 Ir
   (setup 232 incl. ~30 prologue from the unique_ptr param ABI, leave
   ~174, jit_ret ~64, the enter wrapper ~36). vm_frame_setup_lean /
   vm_enter_call_lean (fast_bind + no cache key -> no unique_ptr, no
   coerce branch, one call layer) + vm_frame_leave's cached tail split
   cold. 10_recursion_deep Ir -9.5% (wall -15%), 11 -2.9%, 63 -1.5%;
   fib +0.9% Ir (cached tail's extra layer, wall-neutral). STEP 3
   LANDED same day: the return-side RESULT is MOVED out of the dying
   callee window (LValue::steal_value - jit_ret + both interpreted
   ReturnV paths; the copy was a retain/release pair for a reference
   result), and every callee resolve switched get<intrusive_ptr<
   FuncObject>> -> get_ref (the H1 refcount-churn trap: the by-value
   handle copy was ~28 Ir/call of pure churn, and its dead-at-semicolon
   temporary never protected the callee anyway). 10 -4.7% Ir
   (cumulative -13.8% from pre-lever), 11 -3.6%, 63 -4.6%, 76 -2.4%,
   fib -0.7% (both step-2 costs recovered). STEP 4 LANDED same day
   (the vector-size split): back_rec's records[rec_n-1] was an IMUL by
   the 136-byte record stride at ~23 sites (several per call) - now a
   cached top_rec pointer (native code READS rec_n but never writes
   it, so a fragment can't stale it); records.size() (another per-push
   IMUL), dict_iters/dyn_iters sizes (dyn's 72-byte stride divided per
   push AND pop) are mirrored by plain counters - those vectors are
   mutated only inside push/pop_window. handlers is NOT mirrored
   (fragments push/pop it natively; 4-byte stride = a shift anyway).
   ML_VM_CHECK re-verifies every mirror (the CI-release net). 10 -7.5%
   Ir (cumulative -20.3% from pre-lever; wall -4%), 11 -8.2% Ir, fib
   -1.0%. LEVER 1's C++ side is now ~exhausted: what remains per call
   (~430 Ir on bench 10) is vm_frame_leave's dst put (~29, inherent
   ref-release semantics), pop_window's ref-scan (~29, correctness),
   dispatch stepping, and the protocol's irreducible record/bind
   work - STEP 5 (the fragment-inline sync call)
   LANDED 2026-07-26: CallV/CallValueV emit the depth guard + a flat
   NOEXCEPT push (jit_sync_push_slot/_value -> {window, entry} in
   rax:rdx; arity/overflow/non-fast_bind/dispatch-body are null-return
   pre-checks, no try scaffolding) + a DIRECT `call rdx` into the callee
   fragment (no jit_enter layer) + an inline sentinel test; cold tails =
   the shared jit_sync_postexit (refactored out of jit_call_sync_core's
   direct branch) or the full old helper. Chunk::sync_entry_off (set
   post-JIT) is the direct-entry gate. Measured: 11 -2.2% Ir, 63 -1.4%,
   76 -0.4%; 10 flat (depth>200 runs interpreted - the per-pc-entry
   item), 34/35 flat (VmInvoker path - lever 2); wall neutral within
   bench 11's +-5% swing. Finding both real bugs below outvalued the
   delta: the step's test surfaced a WRONG-CODE call-arg ternary
   miscompile + an inferencer null-type segfault (fixed in the two
   commits preceding it). PER-PC ENTRIES INCREMENT 1 (post-call
   resume) LANDED 2026-07-26: an EnterNative is inserted DIRECTLY after
   every in-VM call op (CallV/CachedCallV/CallValueV) inside a kept
   run, pointing at a per-entry STUB (the head's tag + N5 cache
   establishment, then a jump to the following op's offset) - a runtime
   ret_pc (call pc + 1) LANDS on it, so an interpreted return re-enters
   native mid-run with no lookup and no remap ambiguity (the inserted
   pc is unmapped; bails still reach originals; loc_at(ret_pc-1) still
   hits the call). Proven by g_jit_entry_resume (stub-bumped) + a
   mutual-recursion-past-the-depth-cap jit: test. LEARNED: a
   SELF-recursive CallV is run-EXCLUDED (island), so its following
   run's head already sat at ret_pc - bench 10's resume was ALREADY
   native (the first test draft used self-recursion and the counter
   rightly stayed 0; the honest counter caught it). Measured: a mutual-
   recursion depth bench -1.4% Ir; 10/11 flat; fib's apparent +2% was
   isolated to __strcmp_avx2 ALIGNMENT (identical 698,762 call counts,
   longer per-call path after relink - Ir-level layout noise, worth
   knowing). INCREMENT 2 LANDED same day - REVISED
   from the design during implementation: the handler PC itself is the
   WRONG entry point (CatchTest's native form is exit-at-op, so a
   handler entry would be enter->exit->reinterpret, pure overhead); the
   real gap was the BRANCH TARGET - after one bail/caught throw the
   interpreted back edge targeted remap[head] = the ORIGINAL op, so the
   loop never rejoined native. Increment 2 = branch-target re-entry via
   THE DUAL REMAP: `entry_remap` (branch-target fields + native
   external exits = RESUMES) maps a kept run's head to its head
   EnterNative and an interior target to an inserted EnterNative+stub
   (unified with increment 1's post-call entries - one insertion
   mechanism, entry BEFORE the original); ordinary `remap` (bails,
   exit-at-own-pc, side tables) keeps pointing at originals - a bailed
   op must re-run interpreted, never re-enter (loop). PushHandler/
   CatchTest targets DELIBERATELY stay ordinary. Proven by the -rt
   counter test (>= 1 bump per post-catch iteration) + a -28.1% Ir
   fold-proof try-loop bench (i1 331.9M -> 238.7M; the first bench
   draft self-defeated - auto-pure + const arg folded the WHOLE loop at
   compile time, unwind Ir identical on both sides, counter rightly 0 -
   use runtime(n) in exception benches); 42_exceptions -4.2% Ir; 69 /
   pure loops flat. M5a (THE DEDICATED NATIVE STACK) LANDED
   2026-07-27: a 1GB MAP_NORESERVE reservation (lazy commit; PROT_NONE
   guard page at the LOW end) hosts the native call nesting; the sync
   depth cap became a VARIABLE (g_jit_sync_cap: 200 unarmed, 500k
   armed; emitted guards bake it - the init runs before any emission -
   and jit_sync_push_common's runtime check is AUTHORITATIVE, which
   also lets tests pin it low); the CallV SELF-gate lifts when armed
   (its rationale was the cap round-trip pathology), so self-recursion
   is a native fragment self-call. THE SWITCH PLACEMENT WAR (three
   designs measured): (1) a conditional switch in jit_enter = ~3 Ir
   per FRAGMENT ENTRY = -1% on the callback benches (34/35, millions
   of entries); (2) + a VmInvoker pause did NOT recover it (the check
   itself is the cost); (3) FINAL: jit_enter stays byte-identical-
   plain, the switch lives at the SYNC CALL SITE (emitted, outermost-
   only: cur == null -> plain nested call; else save rsp / switch to
   the baked top / restore around `call rdx` - the restore precedes
   the sentinel branch so the exception path unwinds it) +
   jit_enter_deep for the helper path's direct entry. ASan =
   pass-through (a custom stack trips its machinery; the PoolAlloc
   philosophy); MYLANG_NATIVE_STACK=0 = the same-binary kill switch.
   Measured (Ir vs pre-M5a): 34/35/09/12 FLAT, 10 +2.7%, 11 -1.9%
   (its top-of-chain calls each pay the ~15-instr switched path;
   mitigations recorded: lever 2's direct invoke entry, or an
   ~11-instr switch via a single-base global block). Deep recursion
   (100k) native end to end; overflow stays catchable
   StackOverflowEx; counter-proven (jit_native_stack_deep: >= depth
   inline-call bumps, self AND mutual, + the typed-param helper-path
   assert). M5b (THE FULLY-INLINE RECORD PUSH) LANDED
   2026-07-27, the arc's payoff: emit_sync_push_native emits the whole
   sync push at the call site - callee resolve (gfuncs walk / temp tag
   check), the gate battery, push_window's hot shape (segment fit +
   record REUSE), the ~15-store record fill, the UNROLLED fast_bind
   copies (trivial payloads; a reference arg declines - guarded), the
   captures switch - ending with rdi = window, rdx = entry; offsets
   from JitPushLayout (jit_fill_push_layout, vm.cpp - real members,
   the co-located-probe rule; the FuncObject probe constructs one over
   a static root ctx). EVERY guard precedes EVERY mutation, so every
   decline (undefined/non-func callee, non-fast_bind, arity, overflow,
   segment advance, record HIGH-WATER growth, iter chunks, pending
   cache stash, a reference arg) jumps to the idempotent jit_call_sync*
   tier; the jit_sync_push_* helpers are DELETED. KNOWN SHAPE: a
   first-ever DESCENT grows the record high-water one level at a time
   through the slow tier (cold emplace), so call #1 of a deep recursion
   is all-slow and call #2 flies - the counter test warms up with two
   calls (the single-call draft measured 0 inline calls - the prove-it
   rule again). Measured (Ir vs M5a): 10 -30.4%, 11 -15.5%, 63 -15.6%,
   fib -1.0%, 08/12/34/76 flat; wall (interleaved best-of-9): 10
   0.897x, 11 0.915x, 63 0.903x. CUMULATIVE from pre-lever-1: 10 -46%
   Ir. -vdj verified END TO END (note: the decoder's SIB print shows a
   bogus *8 scale and falls to .byte on sub-from-mem/movsxd - bytes
   hand-verified correct; a decoder polish item). M5c (THE CACHED-CALL FAST PATH) LANDED
   2026-07-27: CachedCallV routes through the inline site - a lean
   jit_cached_probe C++ helper (the map lookup is inherently C++; HIT
   -> dst written, the fragment continues past the call) whose MISS
   parks the constructed key in g_jit_pending_key for the inline push
   to store into rec.cache_key (3 emitted movs, ownership transfer);
   every decline between probe and store falls to jit_call_sync_cached,
   whose core CONSUMES the parked key instead of re-probing (no leak,
   no double probe). The pure-cache handling is PER-SITE: a CACHED site
   stashes the caller's cache into rec.caller_cache inline (a caching
   caller holds a live cache by definition - the M5b decline guard
   would have killed its fast path entirely); a PLAIN site keeps the
   guard (an unconditional stash measured -0.3..-0.5% on 10/11/63).
   TWO lessons paid for in blood: (1) the probe C++ call clobbers
   EVERY caller-saved register - r8/r9 (act/ctx) were not rebuilt
   after it, a RELEASE-ONLY SEGV (dbg helpers happened to preserve
   them; the faulting `mov 0x258(%r8),%r10` under gdb pinpointed it);
   (2) sources were edited while a background battery compiled - the
   battery was discarded and rerun clean (the serialization rule
   applies to BATTERIES too). Measured (Ir vs M5b): fib -2.3%,
   10/11/63 flat (after the conditional stash). emit_sync_call (the
   old plain helper site) deleted - every sync op now emits the inline
   site. Remaining: lever 2 (VmInvoker direct fragment entry -
   12/34/35), the -vdj decoder polish, M6. The "checked-return unwind" half of
   M5 proper PRE-EXISTED (the sync sentinel protocol); interior entries
   also unlock RELAXING the single-entry deletion constraint (an
   external interior branch can enter a stub instead of blocking
   deletion) - recorded for M6.
   (original analysis follows) The single biggest class:
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

2. **Direct fragment calls from callback builtins - LANDED
   2026-07-27.** VmInvoker's ctor caches the callee's fragment entry
   when the body STARTS native (sync_entry_off >= 0 - the whole-body-
   native common case); invoke() then calls jit_enter DIRECTLY per
   element - the per-element vm_dispatch entry/exit + the EnterNative
   op dispatch are gone from the loop. A BOUNDARY sentinel falls
   through to the existing flow read (native ReturnV set flow, native
   Halt left it none - unchanged contracts); a non-sentinel exit takes
   the standalone cold vm_invoke_postexit (vm_raise for a conveyed
   raise - same-frame handler or the boundary-pending conversion
   invoke() already handles; the eptr rethrow; a bail/handler
   continuation via ONE vm_dispatch re-entry, which the old path paid
   ALWAYS). A body that does not start native keeps the dispatch
   fallback; vm_try_invoke (single-shot) deliberately untouched.
   Proven by g_jit_invoke_direct (>= element-count growth over
   map/sort + a throwing-comparator round through the postexit raise
   path). Measured (Ir): 34_sort_custom_cmp -13.8%, 35_map_filter
   -15.3%, 67_make_dict -8.1%; wall 0.938x/0.936x/0.975x
   (interleaved best-of-9). 12_higher_order flat - EXPLAINED: its
   calls are CallValueV (func values called directly), already served
   by the sync-inline path, not VmInvoker. fib's apparent -1% was
   __strcmp_avx2 relink alignment again (attribution checked).
   (original analysis follows) sort/map/filter/
   make_dict/find with a native_leaf callback should `call` the fragment
   per element instead of re-entering vm_dispatch through VmInvoker
   (34_sort_custom_cmp 13x, 35_map_filter 10.5x, 12_higher_order 8.8x,
   67_make_dict). VmInvoker already owns the window + captures for the
   loop; the per-element cost to remove is the dispatch entry/exit.

3. **Borrowed read-only slices - INCREMENT 1 LANDED 2026-07-27** (the
   evidence-first scope cut): profiling showed 15's cost was ~27%
   malloc/free + ~16% hashtable insert/erase - the live-slices
   REGISTRATION, not the view itself - so increment 1 is the PoolAlloc
   treatment for SharedObject::slices (the H2 v2 shareddict drop-in;
   elements are raw pointers, node-pointer stability untouched, ASan
   pass-through free). ONE type change. Measured (Ir): 15 -26.5%
   (wall 0.758x), 16_array_slice_write -14.9%, 14 -4.7%, 47 -0.9%,
   13 flat; the pool-ACTIVE debug lane (ASAN=0 UBSAN=0 OPT=0 TESTS=1)
   green. 29_str's residual is NOT slice machinery: ~25% ord()
   builtin calls + the 1-char-SharedStr-per-subscript - lever 4b
   territory (native ord/len), recorded. INCREMENT 2 (the fork's (a):
   LOOP-INVARIANT SLICE HOISTING) LANDED same day - see CLAUDE.md's
   optimizer section for the full gate set (the COW-DETACH content
   hazard, the base_sliceable zero-iteration-throw proof, the
   literal-base case bench 29 needed - its auto-const string base
   inlines to a literal, so fr_base_id is null). Measured: 15 -40.3%
   Ir / 0.612x wall ON TOP of the pool (lever-3 CUMULATIVE: -56% Ir,
   ~0.46x wall), 29 -20.9% Ir, 16/47 flat (their slices are
   iteration-dependent - correctly not hoisted). Lever 3 COMPLETE;
   option (b) (slot-level borrowing) explicitly NOT pursued - the
   residual prize no longer justifies the value-model cliffs. 29's
   remaining gap is ord()+1-char-SharedStr = lever 4b.
   (original analysis follows) A loop-local slice that provably does
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

## Lever 4b - native len() + fused ord(s[i]) (LANDED 2026-07-27)

- `len(x)`, arg statically a non-opt array/str -> the existing ArrLen/StrLen
  op instead of the CallBuiltinV marshal. Stamp: `CallExpr::vm_len_kind`
  (inferencer annotate walk); builtin-ness proven at codegen
  (DirectBuiltinCallExpr + the `len` uid) so the stamp alone is inert.
  TRAP FOUND: the resolver's devirt swap builds DirectBuiltinCallExpr
  field-by-field - a new CallExpr field MUST be copied there or it is
  silently dropped (the first -vd run showed len still in builtin_calls).
- `ord(s[i])` with `Subscript::base_str` (new stamp) + int index -> the new
  OrdCharV op: TypeStr::subscript's wrap+bounds (OOB caret = the
  subscript's), then the raw byte as int. No 1-char SharedStr per char, no
  builtin call. Interpreter body ML_NOINLINE (vm_ord_char); JIT = the
  SubscriptV convey shape (jit_ord_char), deletable.
- Execution-proven: g_jit_op_run[ArrLen/StrLen/OrdCharV] asserts
  (jit_len_ord); jit_ord_char = 44% of bench 30's new profile, the old
  jit_call_builtin (186M Ir) gone.
- Measured (callgrind Ir, same-session A/B, scale 1):
  30_str_index_iterate 639.5M -> 109.4M (**-82.9%**),
  29_str_slice_readonly 350.8M -> 72.7M (**-79.3%**), 31/47 neutral,
  09/10/11 neutral. Suite (one run per binary): my/py geomean
  0.112x -> 0.105x (**8.93x -> 9.53x** vs CPython).

## Sync postexit depth accounting (pre-existing bug, fixed 2026-07-27)

Found by the clang-ASan lane during the lever 4b battery (pre-existing at
HEAD): the emitted sync call site and jit_call_sync_core decremented
g_jit_sync_depth BEFORE jit_sync_postexit, so the postexit's interpreted
vm_dispatch continuation ran depth-UNCOUNTED - a deep recursion whose
bodies exit to the interpreter mid-body stacked one un-capped ~77KB
(clang-ASan) C frame per level -> stack overflow at depth ~100. Invisible
when the native stack is armed (frames land on the 1GB reserve) and
marginal under lean plain frames. Fix: DEC after the postexit on both
paths; plus the sanitized unarmed default cap is 32 (was 200) - past the
cap a sync call falls interpreted (in-VM, flat), so the cap is a perf
knob the correctness lanes don't care about. The jit_post_call_entry pin
is 32 accordingly.

## Lever 4 - dyn-foreach shape specialization (LANDED 2026-07-28)

ForeachDynInit resolves DynIterState::next (a function pointer) ONCE:
- array, non-indexed, single real var: per-skind body - flat int/float/
  bool read the raw scalar and bind through the baked slot0; general/
  strs/structs keep per-element arr_elem_at (strs may promote mid-loop).
- dict, non-indexed, 1/2 vars: bind key/value through baked slot0/slot1.
- everything else (indexed, `_` single var, N-var strict unpack, >2-var
  dict): the generic body, unchanged.
The per-element Next (interpreter case + jit_foreach_dyn_next) just calls
st.next - no per-element targets/shape/nvars re-reads. Soundness: flat
int/float/bool kinds cannot change at runtime (no promotion; wrong-type
mutation throws); every fast body re-derefs the container per element, so
growth/realloc during the loop behaves exactly as the generic body; the
size snapshot and dict iterator semantics are untouched.

GOTCHA: a `var dyn a = range(N)` DESTINATION is GENERAL storage (the
ArrHint dyn-destination rule), so 66-class code runs the gen body; the
flat bodies serve dyn ALIASES of typed arrays. The execution-proof
counter is per-body (g_dyn_foreach_fast[5]) for exactly this reason -
one aggregate counter could not tell the flat bodies from the gen
fallback (dyn_foreach_fast_shapes asserts all five).

Measured (callgrind Ir, same-session A/B, scale 1): 74_dyn_foreach_kv
9.33B -> 8.37B (**-10.3%**, wall -8.6%), 66_dyn_foreach 10.51B -> 9.77B
(**-7.0%**, wall ~flat - 66 is bound by the boxed body arithmetic, the
#60/N7 arc), 20_foreach_unpack / 26_dict_iterate (typed paths) neutral.
The remaining 66/74 headroom is the boxed `s = (s+e) % M` chain
(jit_boxed_binop + puts ~40% of 66), not the iterator.
