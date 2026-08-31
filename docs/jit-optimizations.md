MyLang — the JIT / VM optimization record
=========================================

This is the per-change record of how the bytecode VM and its native
x86-64 tier reached their present shape: what each optimization does,
what it MEASURED, which guard makes it sound, which sabotage was watched
failing, and — for the ones that did not pay — why they were declined.

CLAUDE.md keeps the RULES that came out of this work, because a rule has
to be obeyed before you know you are in the area it governs. This file
keeps the RECORDS, because they only help once you are already editing
the subsystem.

READ THE RELEVANT ENTRY BEFORE CHANGING ITS SUBSYSTEM. Almost every one
of them documents a trap that cost real time to find: a guard that looks
redundant and is not, a counter that proves a path actually ran, a
measurement that came out the opposite of what the mechanism predicted.
Grep for the lever name (`cache`, `fcache`, `telide`, `fread`, `flit`,
`fwd`, `ffwd`, `resreg`, `hoist`, `hoist2`, `mfact`, `cest`, `relent`),
the opcode, or the task number (#56, #74, #78, #88, #92-#96, #103).

KEEP IT IN SYNC. A change to the JIT or the VM's op set updates THIS
file, in the same commit, exactly as CLAUDE.md's doc-sync rule requires
of CLAUDE.md itself.

**THE NATIVE IN-VM CALL STACK (plans/archived/vm-native-call-stack.md, phases A-F
complete).** A VM->VM call (`CallV`/`CachedCallV`/`CallValueV` with a
chunked callee) is a STATE CHANGE inside the dispatch loop, not a C++
call: `vm_enter_call` pushes a **call record** + a frame **window** on the
activation's SEGMENTED slot stack (segments never move - C++ builtins hold
frame pointers across user callbacks, so windows must be address-stable),
binds args with a `fast_bind` copy loop (or the coercing loop for typed
params), switches `chunk`/`pc`/`captures`, and dispatches; `ReturnV`/`Halt`
pop the record and write the parent's result slot directly (FlowState is
gone from in-VM returns; the deleted `LoopBackEdge` was its last reader).
Per-frame state (handler stack, dict/dyn iterator pools, the per-frame
`PureCache` - stashed/restored so the shared view Frame can't leak it
into global memoization) lives in the records as watermarked slices of
shared stacks; the caught-exception/finally-pend state is PER TRY
REGION, not per record (#78 step 2 - see the paragraph after #74's).
**The record stack is
REUSE-based (N6 call-path lean):** `records` is PHYSICAL storage grown only
to the high-water mark, and a live-count `rec_n` indexes it - a push REUSES
the already-constructed record (`records[rec_n++]`), so the ~140-byte
`VmCallRec`'s 3 `unique_ptr`s are NOT default-constructed/destructed per
call (the `emplace_back` construct was ~6% of a deep-recursion profile, and
reuse also kills the vector realloc-move churn - `10_recursion_deep`
**-18%** JIT-off, suite-neutral). A pop frees any owning field the
exceptional/cache path left set (`exc`/`cache_key` reset, `pend` normalized
- RAII kept, the reuse-equivalent of the old `pop_back` dtor) then
decrements; a BOUNDARY push clears the resume fields `vm_enter_call` would
otherwise set, so a reused record carries no stale resume state. The exceptional path is a
FRAME WALK at the activation's single landing pad: dispatch to a handler in
the current frame, else capture that record's backtrace frame (descriptor
name/params + `loc_at(ret_chunk, ret_pc-1)` + the pure tag), pop, flush the
call op's inlined frames, continue - byte-identical backtraces
(differential-pinned). Boundary frames (main, `do_func_call` entries)
convert to the `g_vm_exc_pending` signal as before. Builtin->callback loops
(map/filter/sort's comparator/make_dict/find/make_array) use
**`VmInvoker`** (vm.h): the callee frame is pushed ONCE per loop and each
element just rebinds the param slots through the activation's reusable
invoke context (its OWN FlowState - reusing the caller's would let a
callback's boundary return corrupt the enclosing frame's flow); single-shot
callbacks use `vm_try_invoke` (eval_func's gate). **#60 (b): the dispatch
loop is split into a file-local `vm_dispatch(chunk, ctx, act)`** - the
`vm_run_chunk` entry (EntryGuard + `vm_enter_invocation_fast` + the
`g_current_ctx` CtxGuard) is per-INVOCATION, so `VmInvoker::invoke` /
`vm_try_invoke` (which already own the activation, boundary window, captures
switch, and - set once for the whole loop - `g_current_ctx`) re-enter
`vm_dispatch` DIRECTLY per element, paying ZERO entry setup (the per-element
EntryGuard dtor was 4.6% of a map/filter profile). Measured whole-program Ir
vs the pre-#60 baseline: 35_map_filter **-10.2%**, 34_sort_custom_cmp
**-9.0%**, 67_make_dict **-6.5%**; pure loops neutral (the function split did
not perturb the dispatch code layout - I-count flat, wall-clock neutral).
Runaway recursion throws
the CATCHABLE **`StackOverflowEx`** at the `MYLANG_VM_STACK` slot cap
(default 1M; README) instead of exhausting the C stack. The `code` pointer
is cached LOOP STATE (making `chunk` reseatable killed the compiler's hoist
of `chunk->code.data()` - a double-load per dispatch, front-end-amplified;
refreshed only at the four chunk-change sites). Zero-copy arg binding was
MEASURED AND DECLINED (the bind is ~2 instructions per 1-arg call; the
protocol around it is what costs - see the plan's Phase E verdict).
**LEVER 1 (2026-07-26, plans/archived/native-gap-roadmap.md) - the LEAN
CALL PUSH/LEAVE:** the callgrind split on 10_recursion_deep showed
~500 Ir of protocol per call, ~30 of it just vm_frame_setup's
prologue/epilogue - the unique_ptr<PureCacheKey> PARAMETER drags
exception scaffolding + a fat spill frame into every call even when
always null, and vm_enter_call was a second NOINLINE layer for two
assignments. The COMMON shape (`fast_bind`, no live cache miss-key)
now takes `vm_frame_setup_lean`/`vm_enter_call_lean` (no key
parameter, no coerce branch, ONE out-of-line call; push_window is
SHARED - it measured lean already) from the interpreted
CallV/CallValueV cases and jit_call_sync_core; the return side's
cached tail (the unique_ptr key handling) moved OUT of
vm_frame_leave into the cold NOINLINE vm_frame_leave_cached, gated
on rec.cache_key. Measured (callgrind Ir / wall best-of-7):
10_recursion_deep -9.5% / -15%, 11_closure_counter -2.9% / -4.2%,
63_closures -1.5%; fib +0.9% Ir (the cached return pays one extra
call layer - wall-neutral), 76 +0.5% Ir (typed params aren't
fast_bind, the gate branch buys nothing there). Step 3 (same day):
the return RESULT is MOVED out of the dying callee window
(`LValue::steal_value` - jit_ret + both interpreted ReturnV paths)
and every callee resolve uses get_ref, not get<> (the H1
refcount-churn trap - the by-value FuncObject handle was ~28
Ir/call of retain/release, and the dead-at-semicolon temporary
never protected the callee anyway): a further 10 -4.7% / 11 -3.6%
/ 63 -4.6% / 76 -2.4% Ir, fib recovered. Step 4 (same day): back_rec's
records[rec_n-1] IMUL (136-byte stride, ~23 sites) -> a cached
`top_rec` pointer (native code reads rec_n, never writes - can't
stale it); records.size()/dict_iters/dyn_iters sizes -> plain
mirror counters (those vectors mutate only inside push/pop_window;
`handlers` is NOT mirrored - fragments push/pop it natively);
ML_VM_CHECK re-verifies every mirror. 10 -7.5% Ir (cum -20.3%
pre-lever, wall -4%), 11 -8.2%, fib -1.0%. The C++ side of lever 1
is ~exhausted; the arc continued in machine code: step 5 (the
fragment-inline sync call), per-pc entry points (post-call + branch-
target resume stubs), M5a (the 1GB dedicated native stack - cap
500k, the CallV self-gate lifted, the switch living at the SYNC CALL
SITE after a measured placement war), and M5b (the FULLY-INLINE
record push: emit_sync_push_native emits resolve/gates/push_window's
hot shape/record fill/unrolled fast_bind/captures at the call site,
offsets via the JitPushLayout co-located probe; guards all precede
mutations so declines fall to the idempotent jit_call_sync* tier; a
first-descent grows the record high-water through the slow tier by
design). M5b measured (Ir): 10_recursion_deep -30.4%, 11 -15.5%,
63 -15.6%; wall ~0.90x each; 10 CUMULATIVE -46% Ir from pre-lever-1.
See plans/archived/native-gap-roadmap.md for the full per-step record.
**THE RETURN SIDE (2026-08-01, plans/archived/cpp-gap-extremes.md cause 1).** A
profile of the my/cpp tail found `vm_frame_leave`/`pop_window` the #1 or
#2 SELF cost in every call-heavy bench (10/11/63/76) - 129 Ir per return,
against a C++ return's ~0. Two changes, both of the "the protocol around
the work costs more than the work" family:
(1) **`Chunk::plain_frame`** - a DERIVED flag (never serialized; the
loader recomputes it beside the three counts it is made of, like
catch_uids) meaning the chunk owns no per-frame side state: no try
regions, no dict iterators, no dyn iterators. Such a frame provably moved
NONE of the four watermarks between its push and its pop - only a
PushHandler moves `handlers` and those exist only where n_trys > 0, and
push_window grows the iterator/pend slices ONLY by this chunk's own
counts while every deeper frame's pop trims to its own base (>= ours). So
pop_window's four comparisons became ONE flag test; a VM_HARDENING build
ML_VM_CHECKs all four are really unmoved, so a future op that pushes
per-frame state without a chunk count fails loudly instead of leaking it
into the caller. Also removed a dead `cur_seg` store (it was overwritten
from the new top on every return that had a caller). Measured: 10 -4.5%,
11 -3.0%, 63 -2.3%, 76 -1.4%.
(2) **The leave body is INLINED into its three callers** (`jit_ret`,
`jit_halt`, `vm_leave_call` - each already an out-of-line function, and
NONE inside `vm_dispatch`'s loop body, so the dispatch text does not
grow). `vm_frame_leave`'s own frame was 13 Ir of prologue + 11 of
epilogue against a body doing ~10 Ir of real work: the `EvalValue res`
BY-VALUE parameter is a 32-byte type with a non-trivial destructor, which
drags a spill slot and cleanup scaffolding into the callee - the same
disease as `vm_dispatch_exc_frame` (#82) and the `unique_ptr` parameter
lever 1 removed from the PUSH side. Measured (cumulative with (1)):
10_recursion_deep **-14.0%**, 11_closure_counter **-9.5%**,
63_closures **-7.7%**, 76_funcval_dispatch -2.8%; fib -0.6%,
08_func_call neutral.
The largest remaining item in the return is the `ref_slots`
reference-release scan (**50 of the ~112 Ir left**, ~25 per listed slot:
two DEPENDENT loads - the slot's type pointer, then its tag - which the
value model makes irreducible). Narrowing the LIST was the lever - DONE
2026-08-04 as **C3 increment 1** (plans/archived/typed-invariant-arrays.md): the
one-shot inferencer stamps `ParamDesc::proven_type` (i/f) for an
UN-annotated param that can only ever receive that scalar - a concrete
non-opt non-dyn param of a non-template, never-value-used
(sym !value_used, finfo !value_escaped), global-scope function - and
codegen's param join then excludes it from ref_slots exactly like a
coerced i/f param. Every call path to such a function is
compile-checked (direct CallV/CachedCallV only; `$` is not an
identifier char and specializations()/globals() return NAMES, so an
instance FuncObject is unreachable as a value outside the excluded
uses). Metadata only - bind paths ignore it; serialized (myv v11).
FIXED ALONG THE WAY: value_instantiate_round's redirect never marked
the CLONE's sym value_used - a value-instantiated instance is
reachable as `ops[k]`, dyn-launderable, callable with unchecked args,
and would have been stamped (the pinned ops-array gate asserts ZERO
exclusions there). THE NET is the existing VM_HARDENING pop_window
audit (every-slot-trivial after the scan): the force-stamp sabotage
aborts -rt on the first wrongly-excluded reference. Measured
(Ir/scale, OPT=1 ASSERTS=0): 10_recursion_deep **-7.1%**, 63 -0.7%;
11_closure_counter flat (lambdas not covered - a follow-up), 76 flat
BY DESIGN (value-dispatched = the gate), 08/09/46/01 flat.
**THE REFERENCE-ARGUMENT BIND (2026-08-01).** M5b's fully-inline push
had an arg-triviality GATE - "each arg's current value must be TRIVIAL
(the inline copy is a raw payload copy - a reference needs the helper's
retain)" - so a call passing an array/string/dict/struct DECLINED the
whole inline push and took the C++ slow tier. That is most real code.
Measured on the SAME indirect func-value shape:
`jit_call_sync_value` ran ONCE in 1M calls with an int argument and
1,000,000 times with an ARRAY argument, where the slow tier cost 55% of
76_funcval_dispatch's instructions. M5b's landing numbers (10/11/63) were
all scalar-arg calls, so they were real but its REACH was never checked -
the prove-the-code-ran rule one level up.
The gate is GONE; the decision moved to the copy loop, per argument: a
scalar takes the raw 32-byte copy as before, a reference calls
**`jit_bind_ref_arg`** (vm.cpp), which runs `fast_bind`'s exact
per-argument step (`dst->rebind(src->get())`) so the two paths cannot
drift. It is a CALL and not an inlined refcount bump ON PURPOSE: a SLICE
registers itself in its parent's `slices` set on copy
(SharedArrayObjTempl's copy ctor), so a raw payload copy plus a retain
would corrupt that set - deferring to the real C++ copy is correct by
construction for every reference type, present and future. FOUR pushes
around the call (an even count preserves the site's 16-byte alignment);
rdx/rcx/r9 are read after the bind and the fourth is a PAD, r8 is not
read, and rsi/rax are dead (emit_call_epilogue reloads the type
singletons anyway); the slots base is in callee-saved rbx, which the
helper preserves for free. Execution-proven
by `g_jit_ref_arg_binds` + the `jit_ref_arg_bind` test (array / string /
SLICE, the last with a live-view base write). Measured (callgrind Ir):
76_funcval_dispatch **-11.1%** (1202M -> 1069M; the slow tier 1,000,000
calls -> 1); 10/63 +0.1-0.3% (the per-arg type test moved from the gate
into the copy loop). NOTE a small DIRECT callee is INLINED away by the
optimizer, so the reference bind is reached by indirect/value calls and
by callees too big to inline - the first version of its test exercised
nothing for exactly that reason.

**C4c - THE INLINE FRAME POP (2026-08-04,
plans/archived/typed-invariant-arrays.md route item 3) - the return-side twin of
M5b's inline push.** jit_ret's C++ round trip (globals, steal,
pop_window, dst put) was ~35% of 10_recursion_deep; the common shape is
now EMITTED at the ReturnV/Halt site (emit_ret_native, jit.cpp) and
jit_ret is its SLOW TIER - every guard declines BEFORE any mutation, so
a decline re-runs the whole pop from scratch, byte-identically. EMIT
gate: the returning chunk's own `plain_frame` + `ref_slots.size() <= 6`
(the list must NOT be required empty - a recursion body's call dsts are
always ref-listed, so an empty gate excludes exactly the motivating
shape; the first run's counter read 0). RUNTIME guards: `boundary` is
NOT a decline but a branch to the BOUNDARY ARM (jit_ret's boundary path
- flow->value raw copy + flow->type = ret - emitted inline; NO pop, the
C++ owner pops; Halt's boundary is a bare -2 sentinel with flow
untouched); no `cache_key` (a CachedCallV miss's cache store is a map
emplace); no `caller_cache` / live vframe.pure_cache (the stash
restore); the RESULT, if ref-listed, currently trivial (a reference
result cannot be raw-moved - the slices set would keep pointing at the
dying slot, the jit_bind_ref_arg lesson - so it declines to jit_ret's
proper steal); and the parent dst slot's OLD value trivial (a release
needs C++). A ref-listed slot HOLDING a reference is NOT a decline
either: the release scan is emitted per listed slot as a type check +
a cold `jit_release_slot` call (pop_window's exact per-slot assignment,
slice-unregistration correct by construction) - which is what serves
76's array-param frames inline. GUARD ORDER is DECLINE-FREQUENCY, not
per-guard cost: boundary/cached first - the first version put the
ref-slot guards first and the callback benches read +2.4-3.1% paying a
dead guard walk per element. The mutations mirror pop_window's fast
path: raw 32-byte result copy (guards proved both sides trivial),
resume globals (a native caller ignores them, an EnterNative consumer
reads them - the callee cannot know which entered it), captures, seg
top / used / rec_n / top_rec / view frame / cur_seg; the parent record
is `top_rec - rec_size` (the records vector is contiguous and cannot
realloc during a pop). A VM_HARDENING build emits a `jit_ret_audit`
call first, so the C3 every-reference-is-listed net stays alive on the
SAME emitted path a release runs (never a hardened-only code shape).
Execution-proven by `g_jit_ret_inline` (emitted incs; the
jit_ret_inline_c4c test pins the recursion, Halt, discarded-dst, and
live-reference/release-arm shapes plus the decline-correctness pair);
sabotage-verified: dst guard (LSan leak), the ref handling (ASan UAF),
boundary guard (abort), seg-top restore (abort); recorded UNPROVABLE in
isolation and kept as defense in depth: the cache_key guard (a record
with a cache key always also carries a stashed caller cache today), the
release call (skipping it only DELAYS the release to slot-reuse /
teardown), and the boundary flow-old-value guard (every consumer moves
flow->value out, leaving none). jit_halt legitimately STARVES (both its
arms are inline) - the Halt coverage case accepts g_jit_ret_inline as
"ran natively", the BinOpV-precedent. Measured (callgrind Ir/scale,
OPT=1 ASSERTS=0 both sides): 10_recursion_deep **-27.8%**,
11_closure_counter **-18.6%**, 35_map_filter **-17.1%**,
34_sort_custom_cmp **-15.6%**, 63_closures **-8.2%**,
76_funcval_dispatch **-7.9%**; 09_fib +0.18% (a cached return pays the
record guards then declines - the cache store is inherently C++);
08/01/46 byte-flat per scale.

**Sync-depth accounting (fixed 2026-07-27):** the depth DEC runs AFTER
`jit_sync_postexit` - at the emitted inline site AND in
`jit_call_sync_core`'s direct-entry branch - because the postexit's
INTERPRETED continuation is one `vm_dispatch` C frame per level (~77KB
under clang ASan: per-case locals + redzones, no scoped-local overlap);
decrementing first let a deep recursion of mid-body-exiting fragments
stack un-capped C frames (a clang-ASan-lane stack overflow; invisible
armed - frames land on the 1GB reserve - and marginal under lean plain
frames). A SANITIZED build's unarmed cap is **32**, not 200
(`jit_native_stack_init`'s #else): past the cap a sync call falls
interpreted (in-VM, flat), so there the cap is purely a perf knob.
**`Chunk::ref_slots` (2026-07-18 profile #2):** the audited list of frame
slots that can EVER hold a >= t_str value (non-coerced params + every dst
of a non-`op_writes_scalar` op; a chunk with a use-def BARRIER op lists
all slots). `pop_window`'s and `VmInvoker::invoke`'s reference-release
scans iterate ONLY these — the fib-class all-scalar frame and the
per-element comparator skip the O(nslots) walk; a VM_HARDENING build
re-scans the full window and ASSERTS the list missed nothing (the audit
net, green across the differential). A NEW op that writes a frame slot
must join `visit_use_def` AND be classified in `op_writes_scalar`.
Net (interleaved full-suite A/B): suite parity with the pre-C1 baseline
plus recursion 0.68x, sort/map 0.85x, make_dict 0.88x, fib 0.86x.

**B1/B2 SPECIALIZED ARITHMETIC (`specialize_arith_ops`, codegen.cpp).** 23
per-operator, per-shape variants of `IntBin`/`FloatBin` (Int Add/Sub/Mul/
And/Or/Xor/Shl/Shr x RR/RI, IntModRI nonzero-imm only, Float Add/Sub/Mul x
RR/RI - see the enum comment in bytecode.h), selected by an IN-PLACE
post-codegen rewrite run AFTER `extract_locs` (opcode swap + a lit-first
commutative op's operand swap into RI; no pc shifts, no loc/pool
interaction). Each removes IntBin's inner 11-way `aop` switch AND both
`is_lit` operand-decode branches. Div/mod-by-reg keep the checked IntBin
path; shifts call `bit_shl`/`bit_shr`. Measured: VM-wall geomean -6.3%,
suite 4.09-4.17x -> **4.45-4.48x vs CPython** (01_while_loop -25%,
03_int_arith -20%, mandelbrot -19%, bit benches -18%).

**#60 lever 2 - the BOXED-arith int-int fast path (`vm_num_binop`, vm.cpp).**
The B1/B2 native arith above is for a PROVEN int/float NODE; a DYN/general
operand - or a comparison used as a VALUE (`x % 3 == 0` as a return value has
no native typed-VALUE compare, only the branch form `JumpUnlessIntCmp`) - still
lowers to the BOXED `BinOpV`/`CompoundV`/`CmpV` (plus the compound global/
capture stores + the dyn inc-dec), which paid `num_bin_op`'s promotion-check
chain + an INDIRECT PMF call (`&Type::add`...) + TypeInt's own dispatch. The
~7 sites now route through **`vm_num_binop(a, b, aop)`**: when BOTH runtime
operands are plain int (the common case), it computes the result inline via a
switch on the Op ENUM (the VM has it; the PMF hid it) - a comparison yields int
0/1, which the CmpV caller wraps with `is_true()` exactly as `TypeInt::lt`
does; any other shape (float/bool/string/mixed) falls back to the EXACT
`num_bin_op` PMF path, byte-identical incl. div/mod-by-zero + bad-shift/type
throws (the caller's catch stamps the loc). `ML_NOINLINE` (a single out-of-line
copy - `num_bin_op` no longer inlines at each site, so `vm_dispatch` doesn't
grow; a DIRECT call replaces the removed INDIRECT PMF). Measured (callgrind
whole-program Ir): 34_sort_custom_cmp **-7.4%**, 67_make_dict **-5.6%**,
35_map_filter **-5.0%** (`num_bin_op`+`TypeInt::eq` 6.8% -> `vm_num_binop`
2.9%); pure loops NEUTRAL (they use native IntBin; I-count flat, wall-clock
0.98-1.00). The tree-walker keeps the plain `num_bin_op` PMF path (the
differential ORACLE - an independent arithmetic impl).

**#60 native typed-VALUE compare (`CmpIntV`/`CmpFloatV`, vm.cpp/codegen.cpp).**
M8's native typed compare existed ONLY as a BRANCH (`JumpUnless{Int,Float}Cmp`,
for a loop/if condition); a comparison used as a VALUE (`x % 3 == 0` returned /
`(a<b)+(a>b)` / a predicate func / a sort comparator's `a<b`) still boxed via
`CmpV`. The two new ops read two int/float Operands and write a real BOOL slot
(`write_bool_slot`) - no box, no `num_bin_op`, no `is_true`. Codegen:
`try_native_cmp_value` (in `compile_boxed_expr`'s `Cat::cmp` path) reuses
`compile_int_cond`/`compile_float_cond` to read the operands + the cmp Op from a
2-operand `TypedScalarExpr` (`kind==i/f`), else falls through to the boxed
`CmpV` (a dyn/string operand, a >2-operand chain). NEVER THROW (a compare can't
fault) -> loc- and node-free; the float form uses plain C++ compares (IEEE NaN
semantics match TypeFloat). Classified like `IntBin` in every codegen table
(`op_writes_scalar`, `visit_use_def`, the two retarget lists) so the E1 peephole
can retarget the bool temp into a return/dst slot. **Op bodies are `ML_NOINLINE`
(`vm_cmp_int_v`/`vm_cmp_float_v`) - the LOOP-BODY TEXT RULE:** inlining the two
handlers into `vm_dispatch` cost `55_float_sum` +0.6% I-count / ~3% wall via
code-layout with NO bytecode change (the front-end effect the roadmap A2 notes
warn about); off-frame helpers restore I-count neutrality. Measured (callgrind
Ir vs the lever-2 baseline): 34_sort_custom_cmp **-10.5%**, 35_map_filter
**-11.5%** (the boxed `CmpV`/`num_bin_op`/`is_true` GONE from the profile;
cumulative -20% / -26% from the pre-#60 baseline). Pure-loop I-count flat; a
residual `55_float_sum` wall-clock signal (~a few %, I-count-neutral) is the
unavoidable `vm_dispatch`-growth layout tax any new op pays.

**#60 - the JIT-inline INT-INT fast tier for the BOXED ops (2026-07-28).**
The emitted BinOpV/CmpV/CompoundV site paid a full helper call (~80 Ir:
marshal + EvalValue copies + vm_num_binop + put) even when both runtime
operands are plain ints - the overwhelmingly common shape in a dyn
accumulator loop. The emit now inlines type-tag GUARDS (operand slots'
type word == t_int; an int literal needs none) + payload arithmetic
(op_rr: add/sub/imul/and/or/xor) + the ref-aware store_dst; CmpV is the
CmpIntV setcc shape yielding a REAL bool; CompoundV's fast store writes
the PAYLOAD only (the guard proved the dst already holds an int - nothing
to release, no type store). ANY other shape - a float/bool/string/mixed
operand, the throwing aops (div/mod/shifts), a float/bool literal - falls
to the EXACT jit_boxed_* helper tier, byte-identical incl. carets; guards
precede every mutation so the decline is idempotent. Execution-proven by
`g_jit_boxed_fast` (bumped by the EMITTED fast path; the
`jit_boxed_int_fast` test asserts BinOpV, CmpV+CompoundV, and the
mid-loop int->float guard-decline separately). Measured (callgrind Ir,
same-session A/B): 74_dyn_foreach_kv **-58.6%** (wall 0.63s -> 0.24s),
66_dyn_foreach **-24.6%** (wall -17%); 34/35/62 exactly neutral (their
hot compares were already CmpIntV). Suite my/py geomean 9.44x ->
**9.81x**. **div/mod joined the tier next** (66's `% M` was the excluded
throwing aop): an IMM divisor inlines when it is neither 0 nor -1 (the
IntModRI idiv-trap exclusion); a REG divisor gets runtime 0/-1 guards
DECLINING to the helper (which throws / computes the -1 case exactly as
the interpreter's C++); cqo+idiv, mod's remainder from rdx. 66 a further
**-45.3%** (cumulative -58.7%); suite **10.25x vs CPython - the 10x goal
crossed**. The jit_op_nativized coverage loop accepts the INLINE tier as
"ran natively" for BinOpV/CmpV/CompoundV (their helpers legitimately
stop bumping when the emitted fast path serves int-int - the deeper form
of native, not a gap).

**D1 - `AppendV` (the append/push fast op).** `append(a, x)`/`push(a, x)`
with one value arg emits `AppendV` (CallBuiltinLV's operand layout: the
builtin_calls pool idx, arg0's kind+slot, the value's slot) instead of
`CallBuiltinLV`+rest-run: the handler forms arg0's `LValue*` from the slot
and runs **`arr_append_fast`** (arr.cpp.h) inline - the shared
NEVER-THROWING append core (flat int/float/bool/POD-struct-match/general +
`arr_append_maintain_hash`; returns false for null/non-array/const/
readonly/slice/flat-mismatch) that `builtin_append` itself now uses after
its slice-clone step, so both engines share ONE append implementation. Any
decline falls back to the full `vm_call_builtin_lv_rest`, byte-identical
(the flat-mismatch TypeErrorEx, COW, carets). Measured: VM-wall 0.983,
13_array_append 0.82x, suite 4.49-4.50x.

**F1 - `MathFnV` (typed math-builtin calls).** A float-proven math-builtin
call (`sqrt`/`cbrt`/`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`exp`/`exp2`/
`log`/`log2`/`log10`/`ceil`/`floor`/`trunc`/`float`/`abs`-on-float +
2-arg `pow`) whose args compile as float expressions lowers to `MathFnV`
(`target2` = a `MathFn` selector, bytecode.h): raw operand read
(`read_float_operand` - an int arg promotes), a direct libm call in the
ML_NOINLINE `vm_math_fn` (loop-body text rule), raw `write_float_slot` -
the whole `CallBuiltinV` marshal (per-arg boxed moves into the run, the
arg-buffer copy, ArgLocs, the builtin fn-pointer call, the boxed store)
is deleted. Selected by `try_math_fn` (codegen.cpp) ahead of the generic
lowering; gated on an unshadowed builtin, EXACT arity (a wrong-arity call
must throw -> generic path), and `th == f` on the call node (so
`abs(int)` -> int result and `float("str")` -> parse stay generic). The
op NEVER THROWS (the float builtins have no domain checks - libm NaN/inf
semantics - and arity/type errors are compile-time-excluded), so it is
loc- AND node-free. Measured: 40_math_builtins 0.50x VM-wall (my/py
0.42x -> 0.19-0.20x, ~5x CPython), suite VM-wall geomean 0.999.

**#76 - the UNIFIED div0 caret convention (2026-07-28).** Per-path the
engines agreed but their CONVENTIONS differed: the boxed ladders (both
engines) caret the offending DIVISOR operand, the TYPED paths careted
the whole chain - so when the engines chose DIFFERENT lowerings for
the same code (a comparator body: tw typed, VM boxed via a global
base), the carets diverged. Unified on OPERAND-PRECISE everywhere:
TypedScalarExpr::eval_int/eval_float's div/mod throw with the DIVISOR
element's span, and the codegen's typed IntBin/FloatBin div/mod record
the divisor's loc via the NEW `CgInstr::loc_node_idx` - a
codegen-transient SECOND node handle for the LOC record only, because
the op's inlined-at chain must stay the CHAIN node's (a
substituted-arg divisor can carry a SHALLOWER chain, which dropped
virtual frames - the #75 parity test caught it). extract_locs reads +
clears loc_node_idx UNCONDITIONALLY up front: a peephole FUSION copies
the source Instr struct (IntAddModRI from an IntBin mod), so the field
rides into ops whose extract branch never touches it - a FUZZER-caught
verify_ast_free abort (71/400 diverged; -rt alone was green - the
fuzzer is load-bearing for codegen-field changes). Pinned by the
"typed div0 carets the DIVISOR operand" test + the comparator smoke.

**LEVER 4b (2026-07-27, plans/archived/native-gap-roadmap.md) - native `len()` +
the fused `ord(s[i])`.** `len(x)` whose arg the inferencer proved a
non-opt ARRAY/STRING lowers to the EXISTING `ArrLen`/`StrLen` op (no
CallBuiltinV marshal): the stamp is `CallExpr::vm_len_kind` (1 array /
2 str, set in the annotate walk; COPIED by the devirt swap - the
resolver's field-by-field DirectBuiltinCallExpr build DROPS any
uncopied CallExpr field, the bug the first `-vd` run exposed), and the
codegen's `try_native_len` separately proves the callee is the
UNSHADOWED builtin (the DirectBuiltinCallExpr node + the `len` uid), so
the stamp alone triggers nothing. `ord(s[i])` with a proven non-opt
string base (`Subscript::base_str`, stamped beside base_array/
base_dict) and an int-compilable index fuses to **`OrdCharV`**
(`try_native_ord`): TypeStr::subscript's exact negative-wrap + bounds
check, then the raw BYTE as int - no 1-char SharedStr, no builtin call;
OOB is its only throw (arity/type/1-char are compile-excluded), caret =
the SUBSCRIPT's via the loc side table. The interpreted body is the
ML_NOINLINE `vm_ord_char` (the loop-body TEXT rule + recursion stack
hygiene - an inline case's locals + sanitizer redzones grow EVERY
recursive vm_dispatch frame); the JIT emit is the SubscriptV convey
shape (`jit_ord_char`, cache-aware index via load_operand; conveys ->
deletable). Classified in ALL the tables (visit_use_def,
op_writes_scalar, op_writes_pure_target - which also GAINED the missing
StrLen - the E1 retarget list, the jit convey/classifier lists).
Execution-proven (g_jit_op_run asserts in the `jit_len_ord` test) +
5 dual-engine `lever4b:` tests (slice base, negative wrap, OOB caret,
dyn fallback, shadowed len). Measured (callgrind Ir, same-session A/B):
30_str_index_iterate **-82.9%**, 29_str_slice_readonly **-79.3%**
(jit_call_builtin's ~186M marshal gone; jit_ord_char ~9 Ir/char),
31/47 neutral; suite my/py geomean 8.93x -> **9.53x**.

**H2 v2 - THE unordered_map NODE POOL (`PoolAlloc`, poolalloc.h; the
core in types.cpp - the global-mutable-state home).** A chained
`unordered_map` heap-allocates a ~96-byte node PER INSERT (hash + the
32-byte EvalValue key + 48-byte LValue + next) and frees it on erase.
`PoolAlloc` serves single-element allocations from PROGRAM-LIFETIME
per-size-class free lists over chunked arenas (single-threaded, no
locks; multi-element allocations - the bucket-pointer arrays - pass
through to operator new; arena blocks stay reachable for leak
checkers; teardown-order-safe: the free-list heads are POD). Wired
into BOTH hot maps: the dict's `inner_type` (shareddict.h) and the
per-frame `PureCache` (eval.h). Node-POINTER STABILITY is untouched
(rehash moves only the bucket array), so every held-LValue*/iterator
invariant holds - a pure drop-in; the FLAT open-addressing map was
REJECTED by the maintainer for exactly that stability reason.
**UNDER ASAN THE POOL COMPILES TO PASS-THROUGH** (poolalloc.h: pooled
reuse would mask a node use-after-free from AddressSanitizer - the
RECYCLE philosophy from the other direction), so the ASan lanes keep
their bug-finding power; test a pool-ACTIVE debug build with
`make ASAN=0 UBSAN=0 OPT=0 TESTS=1`. Measured: 67_make_dict 0.770x,
23_dict_insert 0.929x, 10_recursion_deep 0.968x, suite VM-wall 0.987,
my/py 4.70-4.71x.

**POOLING `std::vector`/`std::string` WAS INVESTIGATED AND REJECTED
(2026-07-19).** The idea (extend `PoolAlloc` to the hot ELEMENT vectors -
`SharedArrayObj`'s int/float/general/struct vectors, `std::string`,
boxed-struct fields - to cut malloc churn + bench jitter) does NOT pay,
for two MEASURED reasons: **(1) a custom allocator DEFEATS libstdc++'s
trivially-copyable memmove fast path.** libstdc++ only bulk-`memcpy`s a
vector grow/copy when the allocator is `std::allocator` (the
`__is_default_allocator` check in `stl_uninitialized.h`); a
`vector<int_type, PoolAlloc<>>` reserve/clone/concat falls back to an
ELEMENT-WISE scalar copy loop - **+56% instructions on 17_array_concat**
(callgrind: `__memcpy_avx_unaligned_erms` 35% -> `vector::reserve`
element-wise 37%). So pooling can only ever apply to NON-trivial element
types (`LValue`, `SharedStr`), where the copy is already element-wise -
and there the win is ~0.5% (the malloc is a sliver of a non-trivial
copy). The only way to pool the flat numeric arrays + strings without the
memmove regression is a GLOBAL `operator new`/`delete` replacement (keeps
`std::allocator`, so memmove survives) - a bigger, riskier change
(reentrancy, size-header, global blast radius) that STILL wouldn't help,
because **(2) the bench jitter is SCHEDULING-bound, not allocation-bound.**
The benches that abort the variance gate or need a scale bump
(04_float_arith, 01_while_loop, 26_dict_iterate, 52_cse_dedup) barely
allocate - 04_float_arith allocates NOTHING in its hot loop and still
swings 5%+ run-to-run. Pooling cannot touch scheduling/frequency noise;
`nice` + best-of-N + the adaptive rep gate (bench/run.py) are the only
levers for it. **Net: `malloc` is NOT the value-model bottleneck.** The
suite geomean was flat (0.999x) even before the memmove regression was
scoped out. The real gap to native C++ (my/cpp ~4.6x) is the value
model's per-op cost: **EvalValue boxing/unboxing, intrusive_ptr
retain/release churn, and virtual `Type`-op dispatch** - that is where
performance work belongs, not the allocator. (Don't re-attempt container
pooling; see poolalloc.h.)

**B3 - THE 32-BYTE PACKED `Instr` (bytecode.h).** `sizeof(Instr) == 32`
(static_asserted), down from 56: `slot` and `lit` are mutually exclusive
(is_lit discriminates), so each operand is ONE 8-byte payload (`pa`/`pb`,
default -1 = the old unset slot); the per-operand tag bits live in the
shared `opflags` byte; `Op` is `: unsigned char`. Exactly two
instructions per cache line. `Operand` SURVIVES as the codegen-side
VALUE type (int_lit()/slot_op()/float_lit(), all compile_* plumbing
unchanged) - an Instr packs one via `set_a()`/`set_b()` and unpacks via
`a()`/`b()` (the cold pass-by-const-ref sites); the HOT readers use the
direct accessors `a_slot()`/`a_lit()`/`a_flit()`/`a_is_lit()`/`a_kind()`.
SEVEN ops (the CallBuiltinLV family incl. AppendV, and the chain stores
StoreElem2V/StoreElemChainV/StoreLValueChainV) used the fat Operand's
slot AND lit as TWO independent ints at once - they use the DUAL view
(`set_a_dual(lo, hi)`, `a_dual_lo()`/`a_dual_hi()`: int32 halves of the
payload; an op uses EITHER the plain view OR the dual view, never both).
Measured: VM-wall geomean 0.970 (broad -2-4%), my/py 4.67-4.68x.
**Stage 2: the runtime `Instr` has NO `node_idx` AT ALL** - codegen
builds a `vector<CgInstr>` (`CgInstr : Instr` + the transient handle,
implicitly constructible from Instr so plain emit sites are unchanged),
`extract_locs`/`verify_ast_free` consume it, and `codegen_chunk` SLICES
the Instr sub-objects into `Chunk::code` (specialize_arith_ops runs on
the sliced chunk) - the zero-AST-at-runtime rule is enforced by the TYPE
SYSTEM for instructions. A refactor of this kind is verified by
BYTE-IDENTICAL `-vd` dumps over bench/ + samples/ (which caught
pp_thread still reading the now-empty Chunk::code - a silent peephole
WEAKENING that -rt cannot see; dump-diff output-preserving refactors).

**H3 - join/split reserve + borrow (engine-shared, str.cpp.h).**
`builtin_join` on GENERAL storage (a string array is always general)
borrows each element by const ref (no boxed copy / SharedStr refcount
round-trip per part) and RESERVES the exact result size from a
type-checking pre-pass, so the append loop never reallocates; the flat-
storage branch keeps the kind-aware `arr_elem_at` loop (same TypeError,
empty flat still ""). `builtin_split` pre-counts the pieces (a memchr
scan, no stores) and reserves its LValue result vector. Measured:
47_wordcount 0.882x (my/py 0.51x), suite VM-wall 0.990, my/py
4.59-4.60x; 31_str_split_join ~flat (growth was already amortized there
- the residual is the inherent per-piece slice LValue).

**H2 - the dict/slot-write micro pair (engine-shared).** From bench 62's
callgrind profile (`counts[key] += 1` cost ~1130 instrs; the insert-alloc
hypothesis was wrong - 62 is UPDATE-bound): (1) **`LValue::put` has an
INLINE fast path** (evalvalue.h) - a put with no container back-pointer
(frame slots, dict values, globals, captures - the overwhelmingly common
case) assigns directly; only the array-element COW path calls the
out-of-line `put_slow` (every put used to pay an out-of-line
`get_value_for_put()` call, on every slot write VM-wide). (2) **String
equality has an IDENTITY shortcut** (`str_views_eq`, str.cpp.h): equal
data pointer + size == equal, no memcmp - the hot case is a string-keyed
dict probe, whose stored key is the SAME StrObj as the probing value (the
key freeze returns strings as-is). Measured: 62_dict_word_count 0.896x
(my/py 0.42x), broad -4-10%, suite VM-wall 0.992, my/py 4.57-4.58x. The
design-level flat open-addressing dict (23_dict_insert's node allocs)
stays a maintainer-sign-off item - roadmap H2 v2.

**H1 - STRUCT CREATION (dst-slot reuse + typed member reads).** Two
pieces (plans/archived/vm-performance-roadmap.md H1): (1) **`vm_struct_ctor`
constructs INTO the dst slot with REUSE** - when the slot's current value
is a same-def, non-readonly POD instance with `use_count() == 1` (the
slot's handle is the only owner), the fields are coerced into a stack
buffer (BEFORE dst is touched - throw-safety) and written over ITS bytes:
zero allocations in the steady-state `var p = Point(...)`-in-a-loop
shape (the same overwrite-in-place + COW-guard trick the flat-struct-
array foreach uses; an aliased/const/other-def dst takes the fresh path,
pinned by a dedicated aliasing test). **GOTCHA (fixed 2026-07-19): the
reuse check must read the dst handle via `get_ref<T>()`, NEVER
`get<T>()`** - a by-value `get<>` COPIES the intrusive_ptr, bumping
`use_count()` to 2, so `use_count() == 1` was never true and H1 was
SILENTLY DEAD (a fresh StructObject + `bytes` vector malloc'd every
`var p = Point(..)` iteration, unmeasured until a value-model profile).
The fix restored it: **64_struct_create -28% instructions / 0.16->0.12s**.
Any `use_count()`-based reuse/COW guard has this trap - the sibling
container-literal H1 already used `get_ref`. `coerce_struct_field` is exported
from eval.cpp for the pre-coercion; the fresh path stores the coerced
bytes directly (construct_struct_from_values would coerce twice).
(2) **`LoadMemberInt`/`LoadMemberFloat`** - the MEASURED discovery was
that allocation was NOT the dominant cost: bench 64's body was 5
`member.v` + 6 boxed arith per iteration, because a STANDALONE struct
member read had no typed lowering (only the foreach-array
LoadStructField* and dict DictLoad* pairs existed). The new pair (the VM
analog of the tree-walker's M8 `MemberExpr::eval_int/eval_float`,
`try_member_scalar` in codegen.cpp: th==i/f + `MemberExpr::base_struct`
+ a resolved-LOCAL base) reads a POD field's scalar straight from the
instance's bytes via the member_keys pool; the boxed-struct/dict/const-
member residue falls to the shared `member_read_core` +
`write_scalar_slot` (fallback throws stamped with the pooled member
caret). Measured (both, full-suite interleaved A/B): 64_struct_create
0.095->0.074s (0.779x; my/py 0.63x -> 0.48x), suite VM-wall 0.999.

**STRUCT BAKED LAYOUT (2026-07-26, the 64_struct_create fix; roadmap
lever 4c).** Field access + POD construction resolve at COMPILE time -
NO runtime name scan (`StructTypeDef::slot_of`, a linear interned-name
compare over the fields vector, used to run per member READ/STORE), no
per-field coerce calls in a proven ctor. Mechanisms: (1) the inferencer
stamps `MemberExpr::base_struct_def` + `field_slot` (resolved next to
`base_struct`); the TREE-WALKER's eval_int/eval_float/do_eval and the
VM's StoreMemberV (via `MemberKey::bake_def/bake_slot`, set by
add_member_key) pick the baked slot behind a DEF-IDENTITY check, falling
to slot_of on mismatch (a dyn-laundered other-def base stays correct).
(2) `try_member_scalar` bakes byte offset + LOAD FORM into
LoadMemberInt/Float (b DUAL: lo = offset or -1, hi = struct_defs idx
<< 2 | form 0 int/1 float/2 bool/3 int-as-float): the interpreted op
runs `vm_load_member_baked` (type-tag + def check + one byte read), the
JIT emits it FULLY INLINE (guards + `mov rax,[bytes+off]`; guard miss ->
the old helper). (3) `Chunk::ctor_plans` (serializable pool): a
StructCtorV whose fields are all scalar gets a per-field {offset, src
slot, act} plan (act 0 raw int - a bool arg's payload is already 0/1;
1 float via read_float_slot's promote; 2 bool byte). THE src_slot RULE:
a bare resolved-LOCAL id arg is read straight from ITS OWN slot at ctor
time (no staging MoveV into a run) - sound ONLY when EVERY arg is
side-effect-free (construct_no_side_effects; else a later arg's `x++`
would mutate the local before the deferred read - with any impure arg
ALL args stage in source order, the old run semantics; pinned by the
"ctor arg snapshot order" test). Computed args go to a contiguous temp
mini-run recorded in `a` (DUAL: lo = base or -1, hi = count - what
visit_use_def enumerates; direct-LOCAL srcs are < slot_count, invisible
to temp liveness by design). pick_cached_slots takes the CHUNK now (an
act-0 plan src is a countable int USE read cache-aware by the emit - a
pinned loop counter feeds `P(i, ..)` from its register; act-1/2 srcs
and dst are bad; the slow branch flush_cache()s first). The interpreted
`vm_struct_ctor_planned` does raw src-slot reads + direct byte stores
(NO coerce_struct_field, NO EvalValue marshal buffer, H1 dst-reuse
kept),
and the JIT emits the H1 guards (type/def/refcount==1/!readonly) +
direct stores inline, slow branch -> the NEVER-THROWING
jit_struct_ctor_planned - so a PLANNED StructCtorV is op_never_exits
(leaf-safe, deletable); an unplanned one (nested-struct field) keeps
the old path (StructCtorV's b is now DUAL: lo = nfields - every b_lit
reader was updated: visit_use_def, pick_cached_slots, disasm, the
fallback emit). StructObject layout offsets are probed in jit_layout()
(public members, + a vector-data-at-+0 probe `sobj_ok` gating both fast
paths). Execution-proven by `g_jit_member_fast`/`g_jit_ctor_fast` -
counters bumped by the EMITTED code, asserted by the `jit:` test
jit_struct_baked (the helpers bump g_jit_op_run, so the old
LoadMemberInt/Float counter cases were repointed at a BOXED struct,
which stays helper-served). Measured (callgrind Ir, 64_struct_create):
JIT-on 1.09B -> 146M -> 137.8M with src_slot (-87.4% total; the staging
`move r5 = i` gone), interpreter-only 1.388B -> 679M (-51.1%); my/cpp
41.6x -> ~12x (scale-40 best-of-5). 58/65/77 neutral (flat-ARRAY
paths).
The residual vs C++ (~24 Ir/iter) is type-tag two-stores + ref-checks
per dst + float type-guard loads - the N7 unboxing arc, not struct-
specific.

**TYPED TERNARY (M8 + codegen).** `specialize_children` (the M8
specializer's recursion, inferencer.cpp) descends into `TernaryExpr` /
`CoalesceExpr` - previously ABSENT, so a ternary's cond/arms were never
M8-specialized and BOTH engines ran them boxed (the recursion-unroll's
guard ternaries - fib's whole body - were the visible cost). And a
th==i/f `TernaryExpr` VALUE lowers natively (`try_typed_ternary`,
codegen.cpp): a typed-compare condition emits one
`JumpUnless{Int,Float}Cmp` to the else arm (any other condition boxes
to `JumpUnlessTrueV`, arms still typed), both arms compile through the
typed compilers into a common dst via MoveV/LoadImm, and the peephole's
E1 join-move rule retargets the movs away - so fib$0's unrolled body is
FULLY native (50 instrs of i.bin/i.jmp.ifnot/call.cached; zero boxed
ops). Measured (full-suite interleaved A/B): 09_fib_recursive
0.006->0.004s (**0.67x**), suite VM-wall geomean 0.990 (broad -3-9%
on the arith/call benches).

**THE #9 FUSION BATCH (2026-07-17, the top-10 list's last item).**
Three more pair-profile superinstructions in the peephole's fusion
block: **`IntAddStep`** (an int accumulate tail `s = s + x` fused into
the counted-loop `ForLoopStep` - add+step+test+branch in ONE dispatch;
never fires when a `continue` targets the step, since that pc is a
branch target), **`ForStepElemInt`** (the back-edge `a[i]` load,
indexed BY THE COUNTER, fused into the step: the original load stays
in place for the loop-entry path and the fused op's target lands past
it; the load's OOB caret rides the fused pc - and the ascending scan +
the is_tgt map make it compose safely with JumpUnlessElemInt), and
**`StructFieldAddInt`** (`dst = other + a[i].f`, GENERAL 3-address -
the struct reduction chains adds through temps, so an accumulator-only
shape would never fire; b_dual = field idx + other slot). The batch's
ROOT-CAUSE bonus: `LoadStructFieldInt/Float`, `LoadMemberInt/Float`,
`LoadStructElemV`, `LoadElemBool` were MISSING from `visit_use_def` -
liveness BARRIERS that made every temp look live in a struct-loop body
and silently blocked IntAddModRI there since E4 landed (audit any new
op into that table, not only visit_pc_fields). Measured: 65_struct_
field_sum 0.783x, 02_for_loop 0.75x, suite VM-wall geomean 0.987,
my/py **4.89-4.93x**.

**E4 FUSIONS (in the peephole; plans/archived/vm-peephole.md).** Two profile-
chosen superinstructions (a scratch op-pair profiler counted 760M
executed adjacent pairs over the suite; the distribution is flat, so
only the caret-safe top pairs shipped): **`IntAddModRI`** (`dst =
(a+b) % IMM`, the checksum shape - never throws: imm nonzero +
int32-gated in `target2`, the add wraps; loc/node-free) and
**`JumpUnlessElemInt`** (`if (arr[i]) ...` - LoadElemInt +
JumpUnlessTrueV in one dispatch; keeps the LOAD's node in place so the
OOB caret is byte-identical; the elem temp must be liveness-dead on
BOTH successor paths). The fusion rules run inside the peephole's
liveness block; `BinOpV→CompoundV` was REJECTED - two throw sources
with different carets can't share one pc's loc entry. A NEW fusion op
with a pc field MUST be added to `visit_pc_fields` (JumpUnlessElemInt
is). Measured: 68_nested/60_bit_sieve -4%, suite VM-wall 0.981, my/py
4.75x.

**THE POST-CODEGEN PEEPHOLE (`peephole_chunk`, codegen.cpp; design +
field tables in plans/archived/vm-peephole.md).** Runs in `codegen_chunk` BEFORE
`extract_locs` - the load-bearing ordering: the loc/`inline_ctxs` side
tables are built from the ALREADY-compacted code, so the pass only ever
rewrites Instr pc fields (every pool is operand-indexed; `node_idx`
handles ride inside the moved Instr structs). Iterated <=4 rounds, each:
(E1) **MoveV elimination** - backward TEMP liveness (a single-word
bitset over `[slot_count, slot_count+n_temps)`; >64 temps skips; an op
not in the audited `visit_use_def` table is a BARRIER that reads every
temp; when the chunk has handlers every op's live-out absorbs the
handler pcs' live-in, since any throw may resume there), then
`<producer dst=tX>; MoveV d=tX` (adjacent, tX a dead-after temp, no
branch entering the move, producer in the audited `retargetable_dst`
whitelist) retargets the producer to d and deletes the move; (E3)
jump-chain threading, INT-only branch-over-jump inversion (float
compares don't invert under NaN), jump-to-next deletion, reachability
DFS, then compaction with a prefix-sum pc remap over `visit_pc_fields`.
**`visit_pc_fields` is THE single audited pc-field enumeration** (a
"target" field is NOT always a pc - `ForLoopStep::target2` is the
COUNTER SLOT, `JumpUnlessTrueV::target2` the value slot,
`SetPend::target` a Pend enum; the E-v1 fuzzer catch) - a new branching
op MUST be added there, and ALWAYS run tests/nested_fuzz.py after a
codegen-pass change. E2 (temp renumbering) was evaluated + DEFERRED (the
native call stack made per-call temp cost ~nil - see the plan); E4 =
this pass IS the fusion framework (no new fusions shipped). Measured
(full-suite interleaved A/B): VM-wall geomean **0.987**, instrs -4.6%,
MoveVs -31%, fib$0's chunk 68->56; the earlier STANDALONE
threading-without-deletion attempt was a measured DECLINE (+3.2%, 1/77
benches affected - roadmap E3 records it).

**NATIVE x86-64 AOT — N0/N1 (plans/archived/native-aot.md; `jit.{h,cpp}`).** The
incremental baseline tier: `jit_compile_chunk` runs LAST in
`codegen_chunk` (after specialize_arith_ops; a `.myv` load will call it
the same way), finds maximal STRAIGHT-LINE runs (EVERY run compiles — the
old `MIN_RUN` >= 4 floor was REMOVED 2026-07-25: with most ops nativized,
the short runs it excluded were mostly whole TINY bodies — a 2-op
comparator `func(a,b) => a < b` — which become `native_leaf` and get
CALLED directly by a caller fragment, paying no `EnterNative` at all;
measured callgrind Ir: sort_custom_cmp 0.93x, map_filter 0.95x,
bool_reduce 0.97x, loop/recursion benches neutral; any branch or
branch TARGET splits a run) of the never-throwing int tier (the B1/B2
specialized arithmetic, IntModRI/IntAddModRI with the imm 0/-1 idiv-trap
exclusions, imm shifts with negative counts left interpreted,
LoadImmInt), hand-emits each into a per-chunk mmap'd W^X buffer
(`Chunk::native`, move-only, never serialized), INSERTS an `EnterNative`
op at each run head (pc fields + locs/inline_ctxs remapped; the run's
ORIGINAL ops stay in place), and flips the buffer RX. The three
contracts: fragments NEVER throw or call anything that can (frameless
leaves - every exceptional condition, e.g. a negative reg shift count,
BAILS by returning the op's pc, and the interpreter re-executes it,
throwing with the exact caret); reads are the release interpreter's raw
proven-type loads (a bool payload is fully zeroed, so the raw int read
is 0/1); writes to a dst OUTSIDE `Chunk::ref_slots` are TWO UNCONDITIONAL
stores (type singleton + payload - sound: such a slot only ever holds
trivial values), while a ref-listed dst (a reused temp that may hold a
reference NOW - releasing it needs C++) gets a type-check + bail. Layout
facts (LValue stride 48, payload/type offsets via
`EvalValue::jit_payload_off/jit_type_off` + a runtime probe, the int
Type singleton) are baked as immediates. Gated on
`ML_JIT_SUPPORTED` (jit.h - **Linux x86-64 ONLY**, maintainer decision
2026-08-02: a platform is supported once it is TESTED there, and CI tests
Linux. FreeBSD x86-64 after it is tested; Darwin x86-64 NEVER, a
deprecated platform; Windows VM-only for a long time; aarch64 on all
three when that backend lands). ONE macro, because a policy stated in 28
places drifts. Kill switch `-nj` / `MYLANG_JIT=0` (the same-binary A/B
lever); off-platform the tier compiles out and `g_jit_enabled` is always
false, which is the same thing `-nj` selects.
The indirect call into a fragment goes through
`jit_enter` (no_sanitize("function") / gcc no_sanitize_undefined):
UBSan's -fsanitize=function would else read a CFI type signature from
the (absent) word before the fragment and fault on the guard page - a
CI-only crash root-caused via a `setarch -R` (ASLR-off) repro.
**N2 - the NATIVE BACK EDGE:** a run may contain
Jump/JumpUnlessIntCmp/ForLoopStep/IntAddStep and interior branch
targets (`op_is_branch` + `emit_branch`), so a whole int loop iterates
in machine code - internal branches are fragment-local jcc/jmp patched
from a per-run `label[]` (`emit_cond_jump`; signed `cc_for`/`cc_negate`
tables), a target outside the run is an `exit_pc`. NO single-entry
constraint: every interior op survives as its interpreted original, so
an external branch or a bail simply resumes interpreted. Measured
(SAME-BINARY JIT off vs on, the cleanest control): VM-wall geomean
**0.895**, my/py 4.97x → **5.47x**; 01_while_loop 0.190x,
50_autoconst_dce 0.227x, 02_for_loop 0.333x, 06_if_branch 0.450x,
68_nested 0.692x. (Cross-binary A/B tiny-magnitude per-bench deltas are
NOISE - always confirm JIT deltas same-binary via the kill switch.)
**N3 - the SSE FLOAT tier:** FloatBin(add/sub/mul) +
FloatAdd/Sub/MulRR/RI, LoadImmFloat, JumpUnlessFloatCmp lower to
movsd/addsd/subsd/mulsd/ucomisd. A float slot READ type-dispatches
(float -> movsd fast path; int -> cvtsi2sd promote; bool/other ->
BAIL - matching read_float_slot); a WRITE is the two-store (t_float
singleton held in r8, set once at entry when the run has float ops, +
movsd payload). Float ORDERING compares (lt/le/gt/ge; eq/noteq not
eligible) use the ucomisd OPERAND-SWAP trick so an unordered (NaN)
compare correctly does NOT satisfy it and jumps - byte-identical to the
tree-walker's IEEE semantics. div/mod stay interpreted (float div
THROWS on 0; mod is a libm call). Measured (same-binary JIT off vs on):
VM-wall geomean **0.812**, my/py 5.00x -> **5.55x**; 54_mandelbrot
**0.344x**, 55_float_sum 0.867x.

**#56 DELETE-ORIGINALS (started 2026-07-28; the re-purposed model
flip).** The corpus AUDIT (env `MYLANG_DELAUDIT=1`: each non-deletable
run's reason + blocking opcodes to stderr) found 58 kept runs across
bench/+samples - 0 multi-entry (the per-pc entries already cover those),
2 inline-raise, 56 bail-op, led by LoadElemInt/Float (46), the call ops
(38), AppendV (10), the exception trio (16). **Increment 1 -
LoadElemInt/Float fully native:** the inline flat fast path keeps its
guards but every DECLINE (non-array/slice/general-or-wrong-kind
storage/negative wrap/OOB) jumps to a SLOW TIER - jit_load_elem_int/
float (vm.cpp), the interpreter's exact shared core
(vm_load_elem_int/float_core, used by the VM_CASEs too so they cannot
drift) - whose OOB CONVEYS with the op's exc-stamped caret; the
InternalErrorEx net rides g_vm_jit_eptr. No bail, no re-interpret ->
op_fully_native -> the runs' originals DELETE (a flat read loop's chunk
is a bare enter.nat). Execution-proven (g_jit_op_run bumps in the slow
tier; the jit_load_elem_slow_tier test covers slice + negative-wrap
shapes). Measured (callgrind Ir): 15_array_slice_readonly **-32.2%**
(sliced reads run the helper instead of splitting/bailing per element),
18_foreach_array **-10.1%**, 14 -3.0%; 43/46 neutral. **Increment 2 - the
LV-BUILTIN family deletable (AppendV / CallBuiltinLV / LVElem /
LVMember):** their jit helpers already ran the FULL interpreter path
(fast append + the pooled-caret fallback / the shared LV dispatch) and
stamp their own carets from the builtin_calls POOL - collapse-safe by
construction; what they lacked was the classification, a
plain-Exception net (catch(...) -> g_vm_jit_eptr - the noexcept would
std::terminate), and a belt-and-suspenders emit-side exc-stamp. Now
op_fully_native -> an append/sort/pop loop's originals DELETE (an
append loop's chunk is a bare enter.nat). Error parity pinned:
const-rebind + flat-mismatch carets byte-identical through the deleted
form; a THROWING COMPARATOR revealed a PRE-EXISTING (parent-verified,
JIT-independent) caret divergence - the VM carets the offending
DIVISOR `z[0]`, the tree-walker the whole `x / z[0]` chain - FIXED
(#76, next paragraph). Ir neutral (13/34/47 +-0.00%). Corpus:
45 -> 41 kept runs. Remaining blockers: the calls (38),
Catch/Reraise/Throw (12), StructFieldAddInt (5), MultiUnpackV (3).
**Steps 2-4 - the CALLS are deletable (2026-07-28,
plans/archived/model-flip.md "The CALLS deletability design"):** every sync-call
decline is gone. The chunk-less callee (the old AOT-net bail) first
LAZY-tries vm_func_chunk (the interpreted op's own net) then runs the
BOUNDARY call inside the helper (jit_sync_boundary_call - the
interpreted tail verbatim, the pending conversion stamping the baked
site). Past the DEPTH CAP the call SWITCHES interpreted-flat
(jit_call_sync_switch): the interpreted op's exact in-VM push with
`rec.ret_pc` = the call's POST-CALL ENTRY-STUB pc (baked by the emit
via the per-chunk `g_cur_entry_remap`) + the resume globals -> status 3
-> the emitted site returns **JIT_RET_SWITCH ((size_t)-3)**; consumers:
EnterNative (switch chunk/pc - ZERO new C frames, the interpreted-call
shape), the inline call-rdx site + the core's direct branch (dec depth
+ PROPAGATE -3 - their C frames die; the record chain re-enters each
fragment at its own stub), vm_invoke_postexit (branch on -3 FIRST).
g_jit_sync_depth is untouched by a switch (the continuation is FLAT).
Backtrace: `VmCallRec::call_site_packed` (the baked site; zeroed by
both interpreted setups, gated on !sync_stop so an emitted-M5b-push
record can't leak a stale value) preferred by vm_capture_rec_frame - a
deleted run's loc_at(ret_pc-1) would resolve against collapsed pcs.
Classification: the three call ops are op_fully_native (NEVER
op_never_exits); post-call entry STUBS now materialize INSIDE deleted
spans (the only pcs there - the entries/dual-remap/rebuild
generalization), so a call loop's chunk is enter.nat + stub enter.nats
and `call.v` is GONE from -vd (the pin updated). Execution-proven:
g_jit_sync_switch (the cap-4 mutual-recursion + deep-throw test).
**THE BOUNDARY CALL IS NOT EXECUTION-PROVEN, and this line used to claim
it was (corrected 2026-08-05, task #114).** `g_jit_sync_boundary_call`
reads ZERO after the whole suite. Its only trigger is a CHUNK-LESS
callee, and since the no-fail codegen the ONLY chunk-less function is a
template BASE (`is_template_base`) - which the inferencer sets exactly
when the base is NEVER value-used, i.e. when every call to it was
redirected to an instance. The two residual routes the inferencer's own
comment names were both BUILT and neither reaches the emitted sync site:
a D4 overflow (70 struct signatures - the ">64 instantiations" warning
fires, the base runs, the counter stays 0) and an uninstantiable direct
call (a dyn arg, a bottom-element `[]` - both instantiated after all).
Those calls tree-walk, as that comment says, rather than arriving at a
JIT'd call site. So the helper is a live SAFETY NET with no constructible
in-suite trigger - and it must stay: since #56 deleted the interpreted
originals there is no re-run to decline to, so a chunk-less callee
reaching an emitted site with this gone would be a crash, not a
slowdown. Corpus: 41 -> **27 kept runs**; call benches
Ir -0.0-0.4%. Remaining: Catch/Reraise/Throw (12), StructFieldAddInt
(5), MultiUnpackV (3), LoadMemberInt/Float guard-miss carets (3),
JumpUnlessElemInt (3), MapFilterV (2), the inline-raise guard (5).
**The small-batch increment (same day):** StructFieldAddInt joins
op_never_exits (its helper is never-throwing, the add/store fragment-
local); MultiUnpackV / CheckFuncV / MapFilterV / LoadMemberInt/Float
join the convey family (exc-stamps added at their exits - LoadMember's
is a belt over its POOLED member carets; eptr nets added to
jit_multi_unpack/jit_load_member for plain callback/dyn throws).
Corpus: 27 -> **22 kept runs**; 65/35/64 Ir neutral, 73_multi_unpack
+0.9% (the deletion reshapes its fragments - the layout-tax class).
Remaining: the exception trio (12), ForeachDynInit/Next (4, side-table
carets - the same treatment next), JumpUnlessElemInt (3, needs a
branch-resolving slow tier), the inline-raise guard (5).
**ForeachDynInit/Next deletable (same day):** both join the convey
family (exc-stamps at their threw exits; `old_pc` threaded into
emit_branch for Next's). TWO measured traps: (1) a catch(...)'s
exception_ptr temp made -fstack-protector-strong add a CANARY to the
hot Next helper - **+7 Ir PER ELEMENT** (66/74 +3.5-4% Ir,
callgrind-diagnosed via the helper's self cost at constant call
counts) - so the throwing tier lives in an ML_NOINLINE slow twin
(jit_foreach_dyn_next_slow) and the hot helper carries NO EH state,
gated by `DynIterState::next_throws` (only the GENERIC body throws;
the five specialized bodies skip the try entirely, ~+1 Ir/element
residual - the price of deletability); (2) the recurring
vm_dyn_next_dict 503<->564M LTO relink oscillation muddied 74's A/B
again. Also: Init's non-container TypeErrorEx now uses the
tree-walker's exact wording (byte parity; the VM's was ALSO loc-less
in the compiled shape pre-stamp - both fixed). Corpus: 22 -> 20 kept
runs; 66 +0.5% / 74 +2.4% Ir (the flag + the relink swing).
**The #9 FUSIONS deletable (JumpUnlessElemInt / ForStepElemInt, same
day):** both keep their inline flat fast paths, but the shared emit
helpers (emit_elem_base_gate / emit_elem_int_read / emit_flat_int_tail)
gained an optional DECLINE LIST - with one, a failing guard (non-array/
slice/general/wrong-kind) and the out-of-range branch JUMP to a slow
tier instead of bailing/`emit_raise`-ing (the raise's loc_at would
resolve against a DELETED run's collapsed pcs). The tiers:
`jit_elem_int_value` (the shared element read via
vm_load_elem_int_core, conveying) for JumpUnlessElemInt and for
ForStepElemInt's POST-STEP read declines, and the FULL-OP
`jit_for_step_elem` (step + test + read, the interpreted body verbatim)
for ForStepElemInt's GATE decline - because the gate must precede the
step, so a re-entry there would DOUBLE-STEP. Both paths CONVERGE on the
value in rax before the fused branch; the full-op tier returns
0 = fell through / 1 = taken / 2 = threw. Corpus: 20 -> **16 kept
runs**; 43_sieve +0.2%, 56_sieve_bool +0.3% (the extra converge jump),
18/65 exactly neutral. The `jit_delete_originals` test's array-read
case FLIPPED to asserting deletion (it pinned the old bail behavior).
**The native `throw` (#56, the exception trio's first third):**
`jit_throw(val_slot, pc, &locs[i])` runs the interpreted op's exact body
- vm_make_thrown_exc + the SHARED vm_raise - and reports one of three
outcomes: **dispatched** (a same-frame handler; the handler pc is parked
in g_vm_resume_pc and the fragment RETURNS it as an ordinary external
exit - the op already ran, so this is a resume, not a re-run),
**boundary** (the walk stopped at this frame's sync_stop/boundary record
with g_vm_exc_pending set; the fragment returns JIT_RET_BOUNDARY, which
the sync sites now route into `jit_sync_postexit`'s existing pending
CONVERSION - each sync site IS a stop boundary, so it converts rather
than propagating), or **conveyed** (a non-struct value's TypeErrorEx).
Two traps: the raw `ret` paths must `flush_cache()` like exit_pc does
(a stale N5-pinned slot SEGV'd cross-frame throws), and the thrown
object must be STAMPED from the baked LocEntry - vm_make_thrown_exc
builds it loc-less and vm_raise would stamp it from `loc_at(pc)`, but a
DELETED run collapses several ops onto one pc, where loc_at returns the
FIRST entry (the ctor's, in `throw E(9)`). CatchTest/Reraise stay KEPT:
the handler dispatch JUMPS to their pcs (the catch-dispatch redesign is
scoped in plans/archived/model-flip.md, deferred). Corpus: 16 -> **15 kept
runs**; 42_exceptions **-9.7%** Ir (the native raise replaces an
interpreted dispatch per throw), 69 -0.9%, 10_recursion neutral.
**The FINAL batch (same day) - 15 -> 7 kept runs corpus-wide:** the
struct/unpack builders (EmplaceStruct, MakeStructArrayV, the four
UnpackElem variants) join the convey family (exc-stamps + the
catch(...) eptr nets; their per-field/arg carets already ride their own
pools), and the INLINE-RAISE GUARD RELAXES from "the run has any
inline_ctxs entry" to "the run has entries naming DIFFERENT chains":
the hazard was distinct chains merging onto the collapsed pc, so a run
whose entries all name the SAME chain (the common shape - one inlined
body spliced as a unit) is safe, since every entry remaps to the head
EnterNative with that one correct index. Ir neutral (58/20/75/09
+-0.01%, 77 +0.23%); the backtrace THROUGH a deleted inlined chain is
pinned byte-identical (jit_final_batch_deletable).
**MakeDictV + the FINAL audit (2026-07-29) - the corpus is 11.** The "7"
above predated samples/phonebook COMPILING again (the value-template
inference fix the same day), so phonebook had contributed nothing to the
audit; with it the corpus is 12, and one of its runs exposed a real gap
the batch had missed - **`MakeDictV`**, the dict-literal builder. Its
`MakeArrayV` twin is op_never_exits ("no error path"); a dict differs
only in that it FREEZES and HASHES each key, so an UNHASHABLE key (a
func) throws. Given the ordinary convey treatment - `emit_exc_stamp` on
the failure branch + a `catch (...)` -> eptr net in jit_make_dict - it
joined op_fully_native; the unhashable-key error is pinned byte-identical
across engines AND through a `.myv` image. Corpus 12 -> **11**.
**DISTINCT INLINE CHAINS (2026-07-30) - corpus 11 -> 9, and the guard is
GONE.** A run whose `inline_ctxs` entries named DIFFERENT inlined bodies
was kept, because deletion collapses every one of those pcs onto the head
EnterNative and the pc-keyed `inline_frame_at` could then flush the WRONG
virtual frames. The fix is ONE store: `emit_exc_stamp` already ran at
every conveying exit INCLUDING the emitted sync call, so it now bakes the
op's chain index (`Exception::jit_inline_frame`, one
`mov dword [rax+off], imm32` on the cold branch, guarded like the caret -
null object, and first-conveyor-wins) beside the caret it already baked,
and `vm_flush_inline` PREFERS it over the pc lookup. The stamp was
restructured so the chain still lands for an exception that carries a
caret but no frames yet, and for an op with a chain but no loc entry.
PROVEN rather than assumed: on a program whose throwing call sits in the
chain listed SECOND, instrumenting the flush printed `baked=1 pc=0
pc_lookup=0` - the pc lookup WOULD have named the wrong body. Pinned in
jit_final_batch_deletable (engines' backtraces equal, the frame is `snd`
and never `fst`, and the `g_jit_inline_baked` counter must bump, so the
test cannot pass by luck or on an unexercised path). `fib$0` is now 9
instructions. NOTE the earlier scoping (in
plans/archived/model-flip.md) predicted
a SECOND mechanism for the call case - a stub-pc `inline_ctxs` entry or a
field on the call record - and this paragraph used to record it as
UNNECESSARY, reasoning that the emitted call site's own exc-stamp runs
before `vm_unwind_walk`'s `inline_origin_emitted`-guarded flush. **That
was WRONG and #88 below is the correction** - the one field can hold only
the RAISE site's chain, so every call site the exception crossed after it
was silently dropped. **All 9 remaining kept runs are now the DEFERRED
catch dispatch** (8 CatchTest+Reraise, 1 EndFinally whose cold reraise
bails).

**#88 - THE CALL SITE'S CHAIN, AND THE -2 SENTINEL (2026-08-02).** With
the JIT on, a recursion inlined into itself rendered FEWER frames than
either other engine (3 where `-tw`/`-nj` render 5) and misattributed the
bottom one: `main()` took the line of the row that went missing, naming a
line inside `f`. Two independent defects, each with its own fix and its
own depth in the pinning test.
**(1) THE CALL SITE HAD NO MECHANISM.** `vm_unwind_walk`'s ordinary pop
resolves a call site's chain from the caller's chunk + the call op's pc,
but its `sync_stop` branch - the record a NATIVE caller owns - cannot: the
sentinel `ret_chunk` is loc-less and the C++ owner is a fragment whose pcs
collapsed onto the head EnterNative. It already stamped the baked call-site
LOC there and simply never stamped the CHAIN. The emitted sync call now
bakes both halves (chain index from the pre-collapse `old_pc`, plus
`inline_frames.data()` - NEVER a `Chunk *`, which dangles once
`codegen_chunk` moves the chunk out) and hands them to the helpers through
**side-channel globals** (`g_jit_call_inline_chain`/`_pool`, the
`g_jit_pending_key` pattern), because `jit_call_sync_core` already uses all
six SysV argument registers and two more would spill on EVERY sync call to
fix a backtrace-only defect. **THE NESTING RULE makes the global safe:**
each helper copies both into its own frame AT ENTRY, before it can dispatch
a callee that would overwrite them, and `jit_call_sync_core` re-publishes
its copy before delegating to `jit_sync_postexit`. The shared
`vm_jit_stamp_call_site` performs the loc stamp and the flush together so
the pair cannot drift apart at one of the five exits.
**(2) `-1` MEANT TWO THINGS.** `Exception::jit_inline_frame` used -1 for
both "no fragment baked anything" and "a fragment baked: no chain here", so
the flush fell back to a pc lookup that, on a deleted run, invented a chain
belonging to another op - a PHANTOM virtual frame. `emit_exc_stamp` now
stamps **-2** for "this op is not inlined code", and `vm_flush_inline_walk`
treats it as an answer. The two stamps have DIFFERENT first-wins guards and
that asymmetry is load-bearing: a real chain stamps whenever the field is
negative (so it still outranks a -2), while -2 stamps only over -1 - an
exception can be conveyed by an op that is not the one that raised, and
that conveyor's "I am not inlined" says nothing about the raise site.
Getting this backwards made the whole batch stamp -2 first and blocked
every real chain (caught immediately by `jit_final_batch_deletable`'s
counter assertion).
MEASURED, not assumed: the first version fixed only the emitted-inline
call and moved the repro by ZERO frames - instrumenting with a distinct
sentinel proved this shape is served by the SLOW tier
(`jit_call_sync_core`), so the side channel had to reach that too. Pinned
by `inlined_recursion_backtrace_parity`, which now runs the JIT ON at
depths 2/3/4 and requires byte-equality with the tree-walker; **each defect
was reintroduced and the test confirmed failing, at DIFFERENT depths**
(depth 4 catches the missing frames, depth 3 the phantom one), so neither
depth is redundant. `g_jit_inline_call_baked` is the execution proof for
the new path, and `jit_final_batch_deletable` now accepts EITHER baked
counter - #88 legitimately moved its shape from the raise-site field to the
call-site channel, which is where a frame for a call inside an inlined body
belongs.
COST (callgrind Ir, `OPT=1 ASSERTS=0` both sides): everything EXACTLY
neutral except **69_exc_crossframe +1.58%** - 42/70/72_exc, fib and
10_recursion_deep all +-0.01%. Two gates got it there from an initial
+1.92%/+0.82% spread, and both are the same idea - do nothing where nothing
can go wrong: the **-2 marker is emitted only when the chunk HAS inlined
ops** (in a chunk with none, the pc lookup it defends against returns -1
anyway, so the block was dead weight on a cold path every conveying op
carries - this alone recovered 70_exc_runtime_error to zero), and the
**side-channel stores are emitted only when the site HAS a chain**, which
is safe because each helper CLAIMS the pair (reads and RESETS it) and a
store always sits immediately before its own call - so a site with no chain
can only ever observe the cleared value. The residual on 69 is the -2 stamp
running per frame-crossing in a chunk that does inline; 69 throws 20k
CAUGHT exceptions across 16 frames each, i.e. the worst case for any
per-crossing bookkeeping, and it is the only bench that moves.

**N4 - flat array element READS:** LoadElemInt/LoadElemFloat lower to a
fragment that navigates the base slot -> SharedObject -> kind + the flat
vector's data/finish pointers, unsigned-bounds-checks the index, and
reads the raw scalar (`mov rax,[rcx+r9*8]` / `movsd`). A non-array,
SLICE, wrong-kind (bool/general/str), OOB, or negative-index base BAILS
(the interpreter re-runs the op with its exact OutOfBounds/type throw +
caret). The fragile SharedObject layout is obtained via a co-located
`SharedArrayObj::jit_probe()` accessor (sharedarray.h) that reads the
real members - so the JIT bakes RUNTIME-correct offsets that can't
silently drift. Measured (same-binary JIT off vs on): VM-wall geomean
**0.897**, my/py 5.05x -> **5.7x**; 18_foreach_array 0.650x,
19_foreach_indexed 0.565x (foreach-over-array is a counted loop +
LoadElemInt), sieve/matrix reads 0.92-0.95x.

**`-vdj` - the post-JIT dump (jit disassembler, disasm.cpp).** Like
`-vd` but each `enter.nat` line is followed by its FRAGMENT's x86-64
disassembly - hex bytes + mnemonics, `; vm pc N` markers linking the
native code back to the VM ops it implements, and `slotN`/`slotN.type`
labels for the frame-window accesses. A self-contained decoder for
exactly the forms the emitter produces (unknown byte -> `.byte`, the
next op mark resyncs); slot-window layout (stride 48, payload +0, type
+24) mirrored for the labels. The op-boundary MARKS are recorded during
codegen ONLY when `g_jit_annotate` (set by `-vdj`) - zero cost on a
normal run. The one dev tool that lets a human read the generated
machine code alongside the bytecode.

**THE SLOTS BASE LIVES IN A CALLEE-SAVED REGISTER (2026-08-01,
plans/jit-registers.md step 1).** The frame-slot window used to sit in
`rdi`, which is both CALLER-saved and the ABI's first argument register.
Both halves hurt: a helper call could clobber it, and forming a pointer
argument (`lea rdi, [rdi+off]`) DESTROYED it - so `emit_call_prologue`
pushed it and `emit_call_epilogue` popped it around EVERY helper call.
The base is now **`rbx`**, callee-saved, so a helper preserves it and the
prologue emits nothing for it. A fragment is entered with the window in
`rdi` (from `jit_enter`, an emitted sync `call rdx`, or a `native_leaf`
direct call) and begins with `frag_entry` = `push rbx; mov rbx, rdi`;
every exit ends with `frag_ret`/`exit_pc` = `pop rbx; ret`. **An entry
STUB is an entry too** and gets the same pair, so either way in pushes
exactly once and either way out pops exactly once. Mechanically the
change is ~10 modrm byte constants: every slot access goes through five
Emitter encoders whose addressing byte was `[rdi+disp32]` (mod 10, rm
111 = `0x87`) and is now `[rbx+disp32]` (rm 011 = `MODRM_SLOT`, `0x83`).
**16-ALIGNMENT moved with it and is the part to keep in mind:** a
fragment is entered at `rsp % 16 == 8`, so before this the body was at 8
and a call site needed an ODD number of pushes; `frag_entry`'s single
push makes the body 0 - already call-ready - so a call site now needs an
EVEN number. `emit_call_prologue`'s pad rule (`pad iff ncache is odd`) is
UNCHANGED because exactly one push was removed from it and exactly one
added at entry; the two hand-spilling sites in the inline call push
(around `jit_cached_probe` and `jit_bind_ref_arg`) each lost a live `rdi`
push and gained an explicit `sub rsp,8` pad in its place. **`rdi` is NOT
materialised by the prologue**: a helper whose first parameter is
`LValue *slots` used to get it for free, and now says so with
`slots_to_arg0()` - measured over jit.cpp, 64 of 73 call sites load `rdi`
with something else immediately after, so a blanket move would have been
dead code at seven sites out of eight. `-vdj`'s slot LABELS follow the
base (disasm.cpp's `mem_disp` keys on rbx now), so the dump still reads
`mov r10, i` rather than `mov r10, [rbx+0x30]`. Measured (callgrind Ir,
`OPT=1 ASSERTS=0` both sides): 43_sieve **-1.31%**, 76_funcval_dispatch
-0.75%, 11_closure_counter -0.59%, 46_matrix_mult -0.52%, 63_closures
-0.38%, 10_recursion_deep -0.31%; 01_while_loop and fib EXACTLY neutral;
35_map_filter +0.30% and 34_sort_custom_cmp +0.25% - the callback benches
re-enter a fragment PER ELEMENT, so they pay the entry push/pop more
often than they save helper-call spills. NOTE the plan predicted ~3% on
76 and that was WRONG: the removed `push rdi`/`pop rdi` is 2
instructions per helper CALL, but the added `push rbx`/`pop rbx` is 2 per
fragment ENTRY, and the two nearly cancel wherever entries are as
frequent as calls. **The reason to do it is not this number** - it is
that four caller-saved registers cannot host a register ALLOCATOR (every
value would spill around every call), and the allocator is the next step.

**THE CACHE POOL IS CALLEE-SAVED AND FOUR WIDE (2026-08-01,
plans/jit-registers.md step 2a).** The N5 registers were `r10`/`r11` -
CALLER-saved, so `emit_call_prologue` pushed each one and
`emit_call_epilogue` popped it around EVERY helper call, and a fragment
could hold at most two. They are `r12`-`r15` now (`CACHE_REGS`,
`MAX_CACHED`): callee-saved, so a helper preserves them for free. The
saves move from once per CALL to once per fragment ENTRY - `frag_entry`
pushes the base plus each pinned register (plus an 8-byte pad when the
count would leave rsp mis-aligned, since the body must sit at
`rsp % 16 == 0`), and every exit undoes it. **`emit_call_prologue` is now
an empty named marker** and the epilogue is just the two type-singleton
`movabs`es. The pick necessarily runs BEFORE the entry is emitted, since
it decides which registers the fragment takes over.
**THE EXIT HAD TO BE SHARED, and that is the trap worth remembering.**
`exit_pc` used to INLINE the whole tail - flush the cache, restore, ret -
so its size grew with the pool: at four pinned slots the flush alone is
56 bytes, and the short `jcc` that several guards use to hop OVER an exit
ran out of its 8-bit displacement (a `patch8` assertion, caught by `-rt`).
An exit is now `mov eax, pc; jmp <epilogue>` - a CONSTANT 10 bytes - and
each fragment emits its tail ONCE at the end (`emit_epilogues`), which
also stops ~100 sites duplicating the flush. **TWO** epilogues, because a
barrier'd op deliberately EMPTIES the cache across its emission: such an
op's exit must not flush (the helper already wrote those slots and the
stale registers would clobber its writes), so the emit picks the bare
tail exactly when `cache` is empty. NOTE `jit_enter_deep`'s asm carries
the C `rsp` across the stack switch in **r12**, which is now in the pool -
still correct, but by save/restore rather than by the emitter never
touching it, and the comment there says so. Measured (callgrind Ir,
`OPT=1 ASSERTS=0`, CUMULATIVE with the rbx move): 43_sieve **-5.07%**,
14_array_subscript **-3.20%**, 46_matrix_mult **-2.82%**,
76_funcval_dispatch -0.75%, 11_closure_counter -0.59%, 63_closures
-0.39%, 10_recursion_deep -0.31%; 01_while_loop / 07_nested_loops exactly
neutral; 35_map_filter +0.30% / 34_sort_custom_cmp +0.25% (a callback
re-enters a fragment per ELEMENT, so it pays the entry push/pop most
often). The array/store-heavy benches gain most because their loops make
helper calls AROUND hot int locals - precisely the spill this deletes.
The `>= 3`-uses heuristic still limits how much of the pool gets used; a
LIVE-RANGE allocator is the next step and is what the wider pool is for.

**C2a - THE FLOAT REGISTER CACHE (2026-08-04, xmm4-7;
plans/archived/typed-invariant-arrays.md).** The N5 pool's float half: hot
float LOCALS pin in xmm4-7 (xmm0/1 stay scratch) - parallel accounting
in `pick_cached_slots` (usef/badi/badf; the pools are DISJOINT: an int
use disqualifies the float side and vice versa), entry loads at the
head + every entry stub, flush (r8 type + movsd payload) / reload /
barrier-bracket extended, and - xmm being ALL caller-saved - a
payload-only spill to the slot around EVERY helper call via the shared
emit_call_prologue/epilogue (sound: a pinned slot is never memory-read
by any op in the run; a real exit flushes type+payload properly).
THE QUALIFICATION RULE: only a slot some float op WROTE in the run
(`fdst`) - a float op can READ a definitely-int slot via the promote
arm, and pinning one would movsd int bits as a double. ReturnV does
not disqualify (the emit flushes before jit_ret); MoveV's SOURCE is
float-aware at ZERO weight (04's accumulator was killed by its final
str(x,4) arg-staging move - the int pool's four-accumulator lesson
replayed); MathFnV joined the classifier (previously UNLISTED - one
math builtin disabled pinning for its whole run). SHIPPED BUG caught
by -rt: `e.fcache` was not cleared per RUN - jit-ineligible selectors
(floor/abs) split a body into fragments and fragment 2's epilogue
flushed fragment 1's never-loaded pin into the slot. Pinned by the
run-split test; 4 sabotages watched failing (no per-run clear, no
spill, no flush - a suite abort - wrong-register read).
Execution-proven by `g_jit_fcache` (emitted inc per float-pinned
fragment entry). Measured (callgrind Ir/scale, OPT=1 ASSERTS=0):
04_float_arith **-28.0%**, 54_mandelbrot **-22.5%**, 55_float_sum
**-19.5%**, 40_math_builtins -2.6%; 44/46/43/01/09/34/35 byte-flat.

**C3 inc 2 - TYPE-ELIDED SLOTS (2026-08-04).** A
qualified-but-unpinned local (pool overflow + sub-threshold - the
same bad() soundness as an N5 pin, minus the register) skips the
per-write TYPE store; every exit's flush stamps the singleton once
(`Emitter::tflush`), the barrier bracket restores it before helpers
that read full values, and an elided FLOAT slot's read skips
emit_float_load's dispatch (provably t_float). THREE suite-caught
holes, each pinned + sabotage-verified: a ReturnV-only slot (an ARRAY
result) qualified - the >= 3 pin threshold had silently protected the
pool from that, so the elision gate is int-WRITTEN-in-run (idst);
MoveV's cache-aware SOURCE is a full-value memory read (stale type
propagation) - sources leave both elision sets (full_read); and the
barrier bracket fired only on a non-empty INT cache - a
closure-capture snapshot of an elided dyn local read a `none` type
(the bracket now fires for float pins + tflush; float pins had been
accidentally safe via the prologue payload spill). Proven by
g_jit_telide; the test needed runtime() armor (const-arg pure fold -
trap #2 again). MEASURED: corpus BYTE-FLAT (hot writes are pinned or
forwarded already); a 6-accumulator probe (more hot ints than the
pool) reads **-6.8%**. Lambda-param coverage was assessed and DROPPED
as vacuous (11's closure has NO params; every passed lambda is
value-escaped by design - recorded in the plan).






**C4b inc 2 - THE FLOAT RESULT PICKS ITS REGISTER (2026-08-04).**
SSE2 arithmetic is TWO-operand (`dst = dst OP src`), so one operand must
occupy the result register. Forcing that to be xmm0 meant a value
ALREADY in xmm0 - a C4a-ii forwarded temp, which is every pair in a
float chain - had to be moved ASIDE before `a` could land there.
`farith` takes its DESTINATION as a parameter now and
`emit_float_operands` returns the `{dst, src}` PAIR: with b forwarded,
`a` is built in the OTHER scratch and the result lands there, so the two
scratches ALTERNATE down the chain and the aside-move is gone.
`JitFwd` carries the register (`fin_reg`/`fres_reg`) rather than
assuming xmm0. The `a`-forwarded case computes over the forwarded
scratch in place (its temp is dead), and `t OP t` needs no load at all
(`dst == src == its register`). fmod is the one forced shape - its libm
call wants x/y in xmm0/xmm1 by the SysV float order.
**THE BUG THIS COST, and the net that caught it:** jit_put_float takes
its value in XMM0, and `emit_float_store`'s ref-listed arm passed
whatever register it was handed - so once the result could be xmm1, that
arm stored a STALE xmm0. `-rt` stayed green; **bench/my/55 was off by
1.5**. That is the second time in one day a corpus program caught what
the suite could not, so the corpus differential (tw vs the default
engine over bench/ + samples/, 83 programs) is now part of the routine,
not an afterthought. Reproducing it in a TEST took real work - the
trigger needs a reference living in exactly the temp whose op has a
forwarded b, which depends on slot allocation: a local-bound array
shifts the numbering and the case goes vacuous, `argv` is empty under
`-rt`, and `clone([...])` misses while `dynarray([...])` hits. Hence
`g_jit_fstore_movx0`, an emitted-code counter the test ASSERTS, so the
coverage is provable rather than lucky.
Measured (Ir/scale, OPT=1 ASSERTS=0): 55_float_sum **-6.7%**,
54_mandelbrot **-3.1%**, 40_math_builtins -0.8%, 46_matrix_mult -0.02%;
04/01 byte-flat.

**C4b - FLOAT LITERAL REGISTERS + REGISTER ARITH SOURCES (2026-08-04).**
Two halves of one idea - a value that already lives in a register should
be READ there. (1) **`farith` takes its SOURCE register as a parameter**
instead of hardcoding `xmm0, xmm1`: SSE arithmetic reads any xmm
directly, so an operand b that is a C2a-pinned local or a pinned literal
needs no `movsd xmm1, xmm<n>` first. `emit_float_operands` RETURNS where
b landed, and the div0 bit test reads that register too
(`movq_rax_x`). (2) **A per-run FLOAT LITERAL POOL in xmm2/xmm3**: a
literal costs TWO instructions to materialise (`movabs` + `movq`) at
EVERY use, i.e. per ITERATION for the most loop-invariant value there
is - 8 of 55_float_sum's 55 hot-path instructions. Loaded once at the
fragment entry (and at every entry stub), an operand-b literal is then
FREE and an operand-a one is a single move.
**THE GATE IS LOOP-SCOPED, and that is the whole accounting**: a use or
a clobbering call INSIDE a loop costs per iteration, outside it costs
once. Scanning the whole RUN instead measured the difference between a
win and nothing - since delete-originals a run spans a whole function,
so `main`'s argv/print calls, long before the loop, declined the pool on
55's real bench shape while the identical loop inside a function pinned
fine. **CORRECTNESS is `emit_call_epilogue`'s job, NOT the gate's**: the
pool is caller-saved and a call can be RUNTIME-CONDITIONAL and invisible
to any opcode scan (a float store to a ref-listed dst calls
jit_put_float and CONTINUES), so the choke point every helper-call
emission already pairs through re-materialises the pool exactly as it
does the rsi/r8 type singletons - verified by turning the gate off
entirely and watching the suite stay green. Through **RCX, never rax**:
the epilogue runs immediately before every call site's `test rax, rax`,
and materialising through rax silently destroyed the helper's status
(it surfaced as a spurious DivisionByZeroEx).
**A LATENT C4a-ii BUG SURFACED HERE AND IS FIXED:** `emit_float_store`'s
ref-listed COLD arm calls jit_put_float, which TAKES its value in xmm0
and leaves it clobbered - so a producer whose pair armed the forward
handed the consumer garbage. The int side had solved this from the
start (`store_dst`'s `keep_rax` reload); the float twin shipped without
it. Now `keep_x0`, cold-arm only, so the hot path pays nothing. It bites
only when a ref-listed float dst actually HOLDS a reference at the
store, which `main`'s temp reuse produces and `-rt` did not - the shape
is now a test (reproduced down to the named bound local: with the bound
inlined an int op writes the temp first, releases the reference, and the
case goes vacuous).
Proven by `g_jit_flit`; the `jit_flit_c4b` test pins an
order-sensitive two-literal chain, the ref-listed-dst shape, and a
libm loop that must DECLINE. Measured (Ir/scale, OPT=1 ASSERTS=0):
04_float_arith **-25.0%**, 55_float_sum **-9.2%**, 54_mandelbrot
**-6.4%**; 40_math_builtins flat (its loop calls libm - the gate
declining, by design), 46/01 byte-flat. 55's hot path 55 -> 48 in a
function, 75 -> 68 at main level.

**C4d - THE CTOR-DOMINATED MEMBER READ DROPS ITS GUARDS (2026-08-05,
`jit_struct_facts`, codegen.cpp).** A baked member read re-checked
`slot holds a struct` + `that struct's def is D` - 7 instructions -
before every single field read, although in the shape those ops exist
for (`var p = Point(i, i*2); s += p.x + p.y`) a PLANNED StructCtorV on
that very slot has just established both. The read is now three
instructions (`mov rax, p; mov rax,[rax+bytes]; mov rax,[rax+off]`) and
has NO slow arm at all - the guards were the only thing that could
decline, and a raw byte read cannot throw, so the cold helper block is
not emitted either.
**THE SCOPE WAS RE-MEASURED, and the plan's was wrong.** C4d was scoped
as hoisting the CTOR's four H1 guards (2x13 Ir); reading the actual
`-vdj` showed the READS are the bigger and far safer half - 64 has five
of them (2 int + 3 float) against two ctors.
**THE MECHANISM is a forward MUST dataflow, not a peephole and not a C1
region.** Facts are (slot, def) pairs, <= 32, one bit each.
GEN: a planned StructCtorV on slot S with def D generates (S, D) on BOTH
arms - the emitted fast path only rewrites the reused instance's bytes,
and the slow tier `vm_struct_ctor_planned` either reuses that same-def
instance or puts a FRESH `def` one in the slot, so neither can leave
anything else there and neither throws. That both-arms property is what
makes the fact reach the reads with no join subtlety. KILL: any write to
S, from `visit_use_def` - the same audited enumeration E1 and
jit_fwd_info use, with the same "an unaudited op is a BARRIER" contract.
MEET: intersection over predecessors (the fall-through edge plus every
branch naming the pc via `visit_pc_fields`, the audited enumeration since
a "target" is not always a pc); an over-enumerated predecessor can only
SHRINK the set, so that direction is the safe one. BOTTOM at pc 0, every
handler body/finally pc, every per-pc entry stub, and every UNREACHABLE
pc - the iteration starts at TOP so a loop-carried fact survives the back
edge, and without the reachability pass an entry-less strongly-connected
region could keep a fact alive forever (it can never run, but it would
still be EMITTED).
**WHAT THIS DOES NOT COVER, and why:** the CTOR's own guards. At a loop
head the fact dies in the meet between the preheader (where the slot has
not been constructed yet) and the back edge - correctly, since iteration
1 really must check. Getting them needs loop VERSIONING (a preheader +
cold twin, C1's structure), which is a separate mechanism; ~26 of 64's
remaining 194 Ir.
**THE SABOTAGE RESULTS ARE THE INTERESTING PART, recorded as measured.**
Forcing the elision unconditionally fails TWO tests (the param read in
`jit_member_fact_c4d` must stay guarded; `jit_struct_baked`'s too), and
removing the KILL fails the rebind case. But the VM_HARDENING net
(`jit_member_fact_audit`, which re-checks the proved fact at runtime on
the SAME emitted path a release takes) does NOT fire under either, across
`-rt` AND the corpus - because for a baked read the base's STATIC type
already implies the def, so no constructible program can put a different
one there. That is not a hole in the nets: it means the guards were pure
insurance against a checker hole, and the elision REPLACES "trust the
checker" with "a ctor just put a def-D object in this slot", which is
strictly stronger evidence. The dataflow and the KILL are what keep that
true if a future op can rebind a struct-typed slot; the entry-pc bottom
is belt (a post-call resume's fact is valid anyway, since the callee has
its own window) and is recorded as sabotage-unprovable.
A TRAP worth naming: the first KILL test case used `p = mk(i)` and was
VACUOUS - a small callee is inlined/spliced away long before codegen and
the reads then read the inlined body's OWN slot, so the no-KILL sabotage
changed nothing (4 emitted def guards either way). The MoveV form
`p = q` is the one that measures (4 -> 2). Execution-proven by
`g_jit_member_noguard` (the guarded `g_jit_member_fast` counts the old
form, so only a separate counter can prove the elided one ran) and by
requiring BOTH counters to move in one program - asserting only the
elided one would pass just as happily if every read in the process lost
its guards. Measured (callgrind Ir/scale, OPT=1 ASSERTS=0, cross-binary):
**64_struct_create 234 -> 194 Ir/iteration (-17.1%)**; 58/65/77 byte-flat
(they read through LoadStructField*/the foreach direct read, not
LoadMemberInt - a different op), 46/01/09/55 byte-flat, so the new emit
arm costs no layout tax.


**C5 - THE LOOP-PREHEADER RELEASE (2026-08-05,
`jit_pick_release_slots`, jit.cpp) - C4e's trick on the STORE side.**
A scalar store to a REF-LISTED slot cannot just overwrite the two
words: the slot may currently hold a reference, and releasing one needs
C++. So every such store emits a 4-instruction test first (`mov rcx,
type; mov ecx,[rcx+t]; cmp ecx, t_str; jb fast`) plus a cold
`jit_put_*` block - on a property that, inside a loop, is false on
every iteration but at most the first.
**`main`'s temps are the case, and they are ref-listed WHOLESALE:**
`compute_ref_slots` bails to "every slot" the moment a chunk holds one
op whose defs it cannot enumerate, which any argv/print call is - so a
hot loop in main pays the test on every store although nothing in the
LOOP puts a reference anywhere. The loop PREHEADER now releases the
slot once (`if (type >= t_str) jit_release_slot`, leaving a trivial
`none`) and every store inside drops its test: ~5 instructions per loop
ENTRY for 4 per store execution.
**THE PLACEMENT WAS MEASURED, NOT ASSUMED.** The plan scoped this at
the FRAGMENT ENTRY; built that way it picked **nothing** on the shape
it exists for - since delete-originals a run spans a whole function, so
main's argv prologue and closing print reuse the very temps the loop
stores to, and "no non-scalar def anywhere in the run" is false for all
of them. Scoped to the loop, the same slots qualify. This is C4b's
loop-scoped lesson, in the same place, for the same reason.
THE GATE: a ref-listed TEMP (locals are not scratch, and
`jit_fwd_info`'s liveness tracks temps only); EVERY def of it inside
the region is `op_writes_scalar` - the same table that decided
ref_slots - so nothing can put a reference back and the invariant holds
at every pc, not merely at the top (an op the use-def table does not
know refuses the whole region); DEAD-IN at the region head, so the
released value is one nobody reads (live-IN, not live-out - C4a-i's
trap); and the preheader must be the only way in, which is
`region_preheader_reached`, FACTORED OUT of C4e so the two cannot
drift. Regions are taken outermost-first (a slot an enclosing region
released is already trivial inside). At an exit the slot holds `none`
or a scalar, so `pop_window`'s release scan correctly skips it -
nothing leaks, because whatever was there was released HERE.
The COLD copy of a C1 hoist region is entered by a failed guard whose
jump precedes this preheader, so it clears the released set and keeps
every guard. `MYLANG_RELDBG=1` prints each region's picks (the
MYLANG_ESTDBG / MYLANG_HOISTDBG pattern).
SABOTAGE, recorded as measured. The DEAD-IN gate needed a REFUSAL
assertion, not a value check: a `foreach`'s counter is a temp that is
live-in AND ForLoopStep-written, so only that gate refuses it - but
`jit_release_slot` assigns `LValue()`, whose payload is ZERO, and a
freshly-initialised counter is zero too, so the wrongly-released loop
still prints the right answer (watched, on the test and on both corpus
programs that expose it). The test therefore asserts nothing was
released. The SCALAR-DEF gate dies as a **LeakSanitizer** report (71KB
in 813 allocations - a reference written into the temp inside the loop,
then overwritten unreleased). `region_preheader_reached` and the cold
clear are recorded UNPROVABLE: removing them changes which slots are
picked (2 sites corpus-wide for the first) but no corpus program's
output, since the failure needs a resume - or a failed C1 guard - to
land where the slot happens to hold a reference right then.
Execution-proven by TWO counters that prove different halves:
`g_jit_release_entry` (emitted preheader) says the release ran,
`g_jit_relent_stores` (compile-time) says a store actually dropped its
guard - a broken `relok()` would leave the first bumping happily.
It also starved `g_jit_fstore_movx0`: that shape got its reference from
main's prologue and the cold arm ran ONCE, which C5 now clears, so the
C4b test grew an IN-LOOP reference and exercises the arm per iteration
instead. Measured (callgrind Ir/scale, OPT=1 ASSERTS=0):
**18_foreach_array -18.2%**, **55_float_sum -12.5%**,
**65_struct_field_sum -11.4%**, **46_matrix_mult -10.3%**,
**14_array_subscript -8.2%**, 64_struct_create -2.4%; 01/43/54/04/34/
35/10/62/15/57/19/42/58/68 byte-flat. Lever `relent`.

**C4e - THE LOOP-ESTABLISHED CTOR (2026-08-05,
`jit_pick_ctor_establish`, jit.cpp).** A planned StructCtorV spends 13
instructions before it can touch a byte - dst holds a struct / same def /
sole owner / not readonly / load the buffer - and inside a loop all four
are true from iteration 2 on. C4d's dataflow cannot reach them: at the
loop head the fact dies in the meet with the preheader, CORRECTLY, since
iteration 1 must really check. **So make the invariant true instead of
proving it.** The loop's preheader calls `jit_struct_ctor_establish`
once (idempotent, and NON-destructive when the slot already holds a
reusable instance - which is what keeps a re-entered inner loop at ONE
allocation for the whole program), and every iteration then emits
`mov rax, dst.payload; mov r9, [rax+bytes]`. There is NO cold twin and
no versioning, because the establish cannot fail.
**THE SAFETY ARGUMENT is placement.** The establish WRITES the slot, so
it must be unobservable. It is emitted before `label[T]` - a back edge
targets the label, so only the FALL-THROUGH entry pays - and a region
with no internal branch and no exiting op runs every op in it, so the
ctor runs and the establish does exactly what that first ctor's own slow
tier would have done. A top-tested `while` is refused by construction:
its T is the condition and the region would contain that branch.
**THE SECOND CONDITION is the appearance scan.** The refcount is what can
break reuse - a MoveV copy, a container store, an arg bind all retain the
instance and H1 would then have to allocate - so the dst slot may appear
in the region ONLY as this ctor's dst and as a baked member read's BASE
(which reads bytes and retains nothing). Both come from
`jit_op_slot_refs`, a thin export of `visit_use_def` added for exactly
this, so the emitter grows no second per-op slot table; an op the table
does not know refuses the region.
**A C4d-ELIDED MEMBER READ COUNTS AS NEVER-EXITING** even though the
opcode does not: with its guards proved away it emits a raw byte read
with no slow tier and literally cannot throw. Using the static
`op_never_exits` there instead refused every real struct loop - 64's five
reads are all elided - so C4e only exists because C4d landed first.
**SABOTAGE STATUS, recorded as measured.** Emitting the establish at the
FRAGMENT HEAD instead of the preheader fails the test - but only after
the zero-trip case was rewritten to take the struct as a PARAM: a local
declared before the loop is re-initialised by its own decl, which masks
the mis-placement entirely (the first version passed). The APPEARANCE
scan is defense in depth today and could not be made to diverge: the ops
that would retain the instance (`append` -> CallBuiltinLV is unaudited,
`arr[i] = p` -> StoreElemValue can throw) are already refused by the
unaudited-op and never-exits gates, and the ones that survive both
(MoveV, MakeArrayV) overwrite their alias every iteration, so no wrong
value results. Execution-proven by `g_jit_ctor_est`; `jit_struct_baked`
keeps the GUARDED form alive with an `if` in the body (a branch refuses
the region), and the jit_op_nativized coverage loop accepts the
established tier for StructCtorV - its helper legitimately starves, the
BinOpV precedent. Measured (callgrind Ir/scale, OPT=1 ASSERTS=0):
**64_struct_create 194 -> 170 Ir/iteration (-12.4%)**, cumulative with
C4d **234 -> 170 (-27.4%)**; 58/01 byte-flat, 46 +3 instructions
whole-program (the picker running at compile time).

**C3 inc 3 - TEMPS JOIN THE FLOAT TYPE-STORE ELISION (2026-08-04).**
inc 2's producer was gated `< slot_count`, LOCALS ONLY, so every float
TEMP still stored its type word on every write. A temp qualifies on the
C4a-i READ gate's condition (dead-in at every entry, so no foreign
value is read through the elided form) **plus one a local does not
need: NOT REF-LISTED.** The flush stamps t_float at EVERY exit,
including one taken before the run's first write to the slot; for a
local that window holds the pre-decl `none` (trivial, harmless), but a
temp is SCRATCH REUSED ACROSS RUNS and can still hold a reference an
earlier run left in the same frame - stamping t_float over its type
word hides it from `pop_window`'s release scan (which tests
`type->t >= t_str` over the ref_slots members) and LEAKS it.
`!ref_listed` is exactly "provably never holds a reference", the
audited invariant a VM_HARDENING build re-verifies over the whole
window on every pop. **That guard is not theoretical: `main` reuses its
low temps for the `argv` subscript AND for a float chain**, so
55_float_sum's own r5/r6 are ref-listed and correctly keep their type
stores while r7/r8 elide - dropping the condition fails the suite AND
trips LeakSanitizer (both watched). Proven by `g_jit_telide_temps` +
the `jit_telide_temps_c3` test (a function-local chain that elides,
and the mixed array/float shape that must not). Measured: 55_float_sum
**-1.3%**, 40_math_builtins -0.6%, the rest byte-flat - SMALL because
C4a-ii landed first and had already deleted 5 of the 7 writes; a
non-adjacent-temp probe (where forwarding cannot fire) reads **-3.1%**,
64 -> 62 hot-path instructions. Reach + architecture, like inc 2.

**C4a-ii - FLOAT DEAD-TEMP FORWARDING (2026-08-04, lever A's twin).**
A float expression chain round-trips every intermediate through a temp
SLOT (a type store + a payload store, then a load) although the value
is alive for exactly one instruction - 21 of 55_float_sum's 67
hot-path instructions. A whitelisted float PRODUCER now hands its
result over in **XMM0**, which is simply where the emitted shape
already leaves it (load a->xmm0, load b->xmm1, arith xmm0, store
xmm0), and a dead non-ref-listed temp skips the store. The whitelist
is the ARITHMETIC family only (FloatBin + the six specialized
RR/RI forms) on BOTH sides - deliberately narrower than the int
lever's, because those ops have **no slow tier that rejoins after
writing the dst** (the div/mod zero arm EXITS), so the "reload the
forwarding register on the slow-path rejoin" case that the int
LoadElem* producers need does not arise. The b-OPERAND case - which is
EVERY pair in the corpus chain, since a chain accumulates on the left
- moves the value ASIDE (`movsd xmm1, xmm0`) before `a` is loaded into
xmm0; the int side instead swaps operand roles for a commutative op,
and that is sound here too (the opcode enum already records that NaN
payloads are not observable in-language), but a 4-byte move needs no
such argument and keeps `sub`/`div` order-correct by construction.
Guards, deadness and the one-shot arming are lever A's, verbatim, on
parallel `fin_x0`/`fprod`/`fskip_write`/`farmed` state (an op is an int
producer or a float one, never both). NOT admitted as a producer: an
op whose dst is float-PINNED - the store is a register move into the
pin and skipping it would strand it; moot today (only TEMPS forward
and the C2a pool is locals-only), so it is an invariant to keep rather
than a runtime check. Execution-proven by `g_jit_ffwd` (bumped by the
emitted consumer); the `jit_ffwd_c4aii` test pins the chain AND an
order-sensitive `a - b` / `a / b` shape, both sabotage-verified (the
aside-move dropped = 10 suite failures; the operand roles swapped =
11). Forcing `fskip_write` unconditionally survives suite + fuzzer -
the SAME honest status as the int side's, recorded not claimed: the
current pairs' temps are consumed exactly once, and the deadness test
is what makes GROWING the whitelist safe. Measured (callgrind Ir/scale,
OPT=1 ASSERTS=0): 54_mandelbrot **-17.4%**, 55_float_sum **-15.4%**,
04_float_arith **-13.9%**, 40_math_builtins -0.6%; 46/01 byte-flat.
55's hot path 67 -> 57 instructions per iteration.

**C2b - A SECOND HOISTED BASE PER REGION (2026-08-04).** A region's
second-best candidate (a dot product's other array - 46's $licm0
beside b) hoists into a CALLEE-saved pair from r12-r15: leftovers
after the int picks, else the two weakest pins DISPLACED when
12 x (region element-ops) beats their whole-run counts (12 = the
measured per-element nav saving; the initial 8x was INERT on 46 -
g_jit_hoist2 == 0, the prove-it-ran rule). Callee-saved: helper calls
preserve the pair (no epilogue re-derive, unlike r10/r11); frag_entry
pushes it; regions share one pair (disjoint lifetimes). All hoist
emit arms go through ONE `hoist_match(base, kind)` lookup; the
preheader is a per-base `nav` lambda; ANY base's failed guard sends
the whole region cold INCLUDING base1 (the body cannot partially
deactivate - a documented trade, pinned). Proven by g_jit_hoist2 +
a wrong-base-nav sabotage (3 cases diverge); the pair-not-saved
sabotage is NOT harness-provable (gcc keeps nothing in r14/r15
across jit_enter here) - recorded. Measured: 46_matrix_mult
**-3.4%**/scale; 14/43/57/18/01/55 byte-flat.

**`MoveV` IS CACHE-AWARE ON ITS SOURCE SIDE (2026-08-01,
plans/jit-registers.md step 2b).** The register pool went four wide and
still would not FILL, and the reason was not the ranking: an op whose emit
touches a slot through MEMORY must DISQUALIFY it (`bad(...)` in
`pick_cached_slots`) for the WHOLE fragment, because a pinned slot's live
value is in a register and memory is stale until the next flush. `MoveV`
did that to BOTH its slots, so one trailing `move r5 = a` - the arg-setup
move in front of any call - cost `a` its register for the entire
fragment. A four-accumulator loop pinned ONE register, its counter.
The SOURCE side is now read from the register: `store_dst(sreg, dst)`, the
same two-store used for any int result (or the release helper when the dst
is ref-listed), and NO reference check on either side. **What makes that
sound is that a MoveV contributes NO WEIGHT to the selection** - it does
not call `usei`, only stops calling `bad`. A MoveV is the BOXED move, so
the bytecode says nothing about the value's type and it can never be the
evidence that a slot holds an int; contributing zero means a slot reaches
the cache only when a genuine int op qualified it, and once it has, every
write to it in the run is an int write - so the value the MoveV reads
really is an int. The DEST stays memory-only for the mirror reason: a
MoveV can write ANY type, so a pinned dst could silently stop holding one.
Measured (callgrind Ir, `OPT=1 ASSERTS=0`): 01_while_loop **-5.92%**,
07_nested_loops **-5.43%**, 03_int_arith **-4.00%**; everything else
within +-0.09% (43_sieve +0.09%, 46_matrix_mult +0.05%, the call and
callback benches exactly neutral). `MoveV` was picked first because
arg-setup moves surround every call; the OTHER `bad()` sites are the same
kind of opportunity and the same treatment applies - each needs its own
argument for why a pinned operand is type-safe there.

**LEVER A - ADJACENT DEAD-TEMP FORWARDING (2026-08-03,
plans/archived/unboxing.md).** A whitelisted int PRODUCER (LoadElemInt,
LoadElem2Int, the specialized IntBin RR/RI family) immediately followed
by a whitelisted CONSUMER (the RR/RI family, IntAddStep) reading its
TEMP dst hands the value over IN RAX: the consumer skips the slot load
(a COMMUTATIVE op with the `b` operand forwarded SWAPS the operand
roles - rax = b OP a - instead of moving RAX aside; only sub pays a
mov), and when the temp is provably DEAD after the consumer and not
ref-listed, the producer skips the two-store entirely. Deadness comes
from **`jit_fwd_info`** (codegen.cpp/.h) - the E1 liveness machinery
(visit_use_def / visit_pc_fields / handler absorption) run at JIT time
on the FINAL post-splice code, so jit.cpp grew no second per-op table;
building it found E1's handler absorption EMPTY since #78 step D
(PushHandler.target went -1; now collected from handler_sites - fixed,
conservative direction, measured neutral). Guards: same run, adjacent
pcs, consumer not a branch/handler target or entry pc, TEMPS only, no
cache barriers; a producer's SLOW tier reloads RAX on its rejoin (the
helper's status clobbers it), and a REF-LISTED dst keeps its write with
the reload in store_dst's COLD ref arm only - the v1 hot-path reload +
move-aside MEASURED the whole yield away (+2 Ir/iter on 46 against the
predicted -2; the scale-delta A/B caught it, the distrust-a-surprising-
result rule in action). The `jit_fwd_consumer` CONTRACT: every op it
accepts must honor `g_fwd.in_rax` in its emit - grow both sides in the
same change. skip_write is defense in depth today (forcing it survives
suite + fuzzer: current pairs' temps are consumed exactly once); it is
what makes GROWING the consumer whitelist safe (a counted loop's BOUND
temp is live every iteration). **And it was INERT until 2026-08-04 for
a reason that had nothing to do with ref_slots: `visit_use_def` did not
know the B1/B2 SPECIALIZED family, so at JIT time - the only place that
family exists - every op was a use-def BARRIER and the liveness read
`all` (see THE AUDIT-TABLE STAGE TRAP below).** Execution-proven by
`g_jit_fwd` (bumped
by emitted consumers; the jit_fwd_deadtemp test pins the chain, the
matmul shape, and the slow-path rejoin - all sabotage-verified).
Measured (callgrind Ir, OPT=1 ASSERTS=0): 46_matrix_mult -2.19%/iter,
14_array_subscript -1.2%/iter, 07_nested_loops -2.83%, 03_int_arith
-1.36% whole-program; the rest <= +0.04% (link noise per the -nj
control). The write elision is throttled by ref_slots' conservatism -
narrowing it is C3 (plans/archived/typed-invariant-arrays.md).

**C1 - PER-LOOP NAVIGATION HOISTING (2026-08-03,
plans/archived/typed-invariant-arrays.md - the typed-invariant staircase's first
step).** A loop's element ops re-derive the base's navigation EVERY
element (type tag, slice flag, shobj, kind, data/finish, count) for a
base that cannot change inside the loop. C1 is LOOP VERSIONING:
`jit_hoist_pick` finds a backward branch's REGION [T, L]
(innermost-first) whose ops are all on a read-only-FOR-STORAGE
whitelist - no calls, no boxed PMFs (TypeArr::add mutates), but PLAIN
element stores are admitted (they never move a non-slice base's
storage; growth is a builtin call, refused) - with no jump into the
region from outside, no entry-stub/handler pc inside, and one
consistent-kind LOCAL base never defined in the region. The PREHEADER
(bytes before label[T] - back edges target the label, so only the
fall-through entry pays) verifies type/non-slice/kind once and derives
(data, count) into **r10/r11 - CALLER-saved**, so the N5 pool is
untouched (two earlier designs died by measurement: run-scoped gating
fired on zero benches - a post-#56 run spans the whole function, calls
included - and reserving callee-saved regs cost every OTHER loop in
the fragment two pins, 43_sieve +3.3%). Any helper call inside the
region clobbers r10/r11: `emit_call_epilogue` - the choke point all 83
helper-call emissions pair through - re-derives both when a region is
active (via RCX; RAX carries the helper's status). A failed preheader
guard jumps to a COLD copy of the region alone (the ordinary emission,
emitted after the run; region-internal branches patch against its own
labels, exits rejoin the shared stream) - never a mid-loop bail, and
deletion needs no interpreted original. The hoisted element forms:
LoadElemInt/Float = bounds-vs-r11 + read-off-r10; the elem2 OUTER
(kind general, byte-length count) = imul+bounds+lea, rejoining the
common row section. Execution-proven by `g_jit_hoist` (the emitted
preheader bumps on success; refusal shapes assert ZERO) +
MYLANG_HOISTDBG=1 (per-region refusal reasons, the DELAUDIT pattern).
Sabotage-verified: the slice guard (a runtime slice read the parent's
elements), the def scan (a mid-loop rebind kept stale registers), the
epilogue re-derivation (ASan SEGV - clobbered r10 as a data pointer).
Measured (callgrind Ir, OPT=1 ASSERTS=0, scale-delta):
46_matrix_mult **-12.0%**/iter (~89.5 -> ~78.7), 14_array_subscript
**-15.9%**/iter; everything else within +-0.01% incl. 15 (a runtime
slice base - the cold twin serves it at no cost).
**C1b - the STORE side (same day).** A region that STORES to its base
(`HoistRegion::has_store`) gets three more preheader guards - const
slot, readonly, has_slices, all region-stable - and a ONE-SHOT hash
invalidation there (hash_valid=0 early only means "recompute later",
so the per-element store needs neither the shobj nor a third
register): the element is bounds + raw write. The store guards are
emitted ONLY for storing regions - unconditionally they would send a
read-only loop over a CONST base cold, losing the read hoisting. Plus
MULTI-REGION (greedy innermost-first, non-overlapping, r10/r11 reused
across disjoint regions - a one-region pick served only 43's tiny fill
loop). Measured: 14_array_subscript a further **-29.0%**/iter
(cumulative -40% across C1+C1b); 46/15/18/01 neutral. 43/56_sieve
STILL do not move and the mechanism is live-counter-proven (the
ordinary #92 tier serves their 3.1M stores; g_jit_hoist 0): their
arrays are BOOLS and the pick stamps StoreElemInt candidates kind
INTS - the Instr does not carry the element kind. **C1c LANDED (same
day, design b - maintainer's pick):** a compile-time ELEM-BOOL hint in
StoreElemInt's previously free opflags bit 6, stamped when a PLAIN
bool-LITERAL store compiles (the checker rejects int->bool, so the one
mislabel - #96's bool into an int-joined array - just fails the kind
guard and goes cold: ADVISORY, semantics never depend on it; no .myv
bump - the opflags byte was always stored whole). The pick maps a
hinted store to kind 3: the preheader guards kind_bools, the count is
BYTES, the hoisted store is a byte write of dil (only 0/1 LITERALS
reach StoreElemInt on bools). Measured: 43_sieve **-45.3%**/scale,
56_sieve_bool **-44.3%**; 46/14 byte-identical; the stride sabotage
(8-byte stores into the byte array) died as an ASan SEGV. **The READ-side
hint landed (same day):** the truth source moved to the inferencer -
`Subscript::elem_bool`, stamped beside base_array from the base's
static type and copied by clone(); the STORE site switched to it too
(one source of truth, no #96 mislabel). LoadElemInt carries the hint,
kind 3's hoisted read is a movzx byte (stride sabotage-verified), and
the former store+read kind CONFLICT now agrees and hoists both ways
(the pinned test flipped). The C1b sabotages each
required defeating a fresh shape-eater first: a bare `runtime()`
argument is DYN, which lowers `arr[j] = n` to StoreElemValue - no
candidate, a vacuous case - `int(runtime(5))` keeps the store
StoreElemInt.
**C1d - the FUSIONS as candidates + TEMP bases (same day).** Three
unlocks, each found by chasing where the previous one's reach ended:
(1) `JumpUnlessElemInt` (the fused sieve test) is a candidate - its
base/idx/hint ride the mutated-in-place load's own fields - and the
hoisted form lives in the SHARED `emit_elem_int_read` (bounds-vs-r11 +
byte-or-int read off r10; declines land on the caller's conveying slow
tier, which reads memory). (2) `ForStepElemInt` is a candidate (base =
b_dual_lo) - but the fusion COPIES the step's struct, so the load's
ELEM-BOOL hint must TRANSFER by hand after set_b_dual (which clears
b's flag bits; sabotage: without it the pick stamps kind ints, the
kind guard goes cold, the counter assertion catches it); its hoisted
form skips the base gate entirely (the preheader proved it - and
nothing bail-able precedes the step, so no double-step hazard, no
SLOW-B) and the post-step read is bounds + read, declining to the
existing jit_elem_int_value tier. Its old def list marked `target` - a
BRANCH PC, not a slot - as a def (harmless-looking, but a pc-numbered
slot was spuriously killed as a candidate); fixed. (3) **TEMP bases
are admitted**: a foreach-over-array snapshots the container into a
TEMP, so every foreach loop has one - N5's temp hazard (eager
entry-load + exit-FLUSH) does not apply to a base that is only READ at
the preheader, the def scan still kills an in-region rebind, and an
aliased store cannot move the storage (#92's has_slices rule). Also
whitelisted: `DictLoadInt/Float` - strictly weaker than the admitted
DictStore (a vivify mutates the dict's own nodes, never an array's
vector) - which unblocked 68_nested's ForStepElemInt regions.
Measured (callgrind Ir/scale, OPT=1 ASSERTS=0): 57_bool_reduce
**-41.6%** (360.6M -> 210.6M), 18_foreach_array **-37.1%** (the
temp-base unlock), 56_sieve_bool **-24.8%**, 43_sieve **-18.2%**
(their count loops joined); 68_nested exactly neutral (its foreach
arrays are tiny); 46/14/01/02/09/15 byte-flat per scale.
**C1e - the hoisted-COMPOUND store (same day; the family's last
rung).** The hoisted store arm serves `a[i] OP= v` too: divisor
0/-1 guards (before bounds - a div0 store must throw in the helper
WITHOUT storing), bounds-vs-r11, then the RMW via `mov rcx, r10` so
the ordinary tier's [rcx+r9*8] tails serve verbatim - minus the
per-element hash store (preheader, once) and the nav. Hint-3
compounds never hoist (compound-on-bools is compile-unreachable).
Execution-proven by the arm's OWN `g_jit_hoist_rmw` (g_jit_store_fast
counts the ordinary tier too - it cannot prove this arm). Corpus
byte-flat (no bench compounds into an element); a 1M-compound-store
probe reads -29.5% (~19 Ir/store). The C1e divisor test EXPOSED a
pre-existing engine-uniform hole, since FIXED - see the #103
paragraph below. Next on the staircase: C2/C3 per the plan.

**#103 - INT_MIN / -1 THROWS (2026-08-03, maintainer ruling:
like division by zero - and NEVER via a signal handler).** It used to
be UB: TypeInt::div/mod and the VM store bodies raw-divided with only
a zero check (-fwrapv covers +,-,* only; x86 idiv raises a hardware
#DE), so `(1 << 63) / -1` SIGFPE'd EVERY engine including the
parse-time const-fold. Now `check_int_div_overflow` (bitops.h - the
shift helpers' home, included by all three TUs) throws the catchable
**InvalidValueEx("integer overflow in division")** at the five C++
sites: TypeInt::div/mod, vm_num_binop's int fast path, the VM element
-store compound switch, the interpreted IntBin div/mod (via vm_raise),
and TypedScalarExpr::eval_int (divisor-span carets, #76). `% -1`
throws too - one uniform rule (mathematically the remainder would be
0; uniformity chosen, C#'s behavior). The JIT needed exactly ONE new
emission - IntBin div/mod's inline idiv (every OTHER emitted division
already carried explicit 0/-1 pre-checks declining to the now-fixed
helpers) - and its COST WAS DRIVEN TO THE FLOOR by measurement: the
first version's second compare+branch per division read +3.4%/+4.8%
Ir on 03_int_arith/44_primes_sqrt, so (a) a LITERAL divisor now
decides at COMPILE time and an ordinary one emits NO runtime checks
at all - not even the zero test the pre-#103 code ran on literals, so
03 lands at **-3.4%** vs pre-#103 - and (b) a SLOT divisor pays ONE
gate: `lea rdx,[rcx+1]; cmp rdx,1; ja .div` catches 0 and -1 in one
branch, the cold block (0 -> JR_DIV0; -1 -> the INT_MIN compare, the
new **JR_DIV_OVF** kind) FALLING THROUGH into the division for a
legitimate x / -1 - 44 lands at **+2.4%**, the +1-instruction floor
for an explicit check. The FOUR copies of the kind->exception ternary
collapsed into ONE `vm_jit_raise_kind_new`. IntModRI / the
IntAddModRI fusion EXCLUDE an imm -1 at selection (their handlers are
uncheck-fast; `% -1` falls to IntBin's checked path). Pinned by 5
five-mode tests (div, mod, catch + the /-1-still-divides case - which
exercises the cold fall-through - the element compound path, the
fully-const BUILD failure) and the C1e -1-guard case now asserts the
InvalidValueEx (previously sabotage-unprovable - the helper crashed
too); the guards sabotage-verified on the final shape (each drop = an
FPE abort). README documents the rule under Integer.
**The -1 REFINEMENT (same day, maintainer-directed):** a runtime -1
divisor no longer declines WHOLESALE anywhere - only the ONE dividend
that overflows does. All four decline-based tiers (the boxed int-int
inline tier, the ordinary + nested element-store tiers, the C1e
hoisted arm) now use IntBin's shape: the hot path is the single
lea/cmp/ja gate; the cold side declines 0, then reads the ACTUAL
dividend - rax for the boxed tier, the ELEMENT for the store tiers
(which is why the store gates moved AFTER their kind proof; the
nested tier's cold path must not clobber rcx = &row, so it forms
&elem in rdx with a TWO-sided pointer bounds compare - a negative
index wraps below data) - and declines ONLY INT_MIN; an ordinary
x / -1 rejoins the native path. Proven by counters (the #95 tests'
fast counts ROSE by exactly the formerly-declined stores; the C1e
INT_MIN case now requires rmw >= 1 - the sane elements before the
throwing one run the hoisted RMW) and by four cold-check sabotages
(each skip = an FPE abort in -rt). Measured: element-compound and
boxed /-1 probes **-52.6% / -54.9%** whole-program; 03/44 byte-flat.
MEASUREMENT-HARNESS NOTE: shopping/phonebook fed </dev/null spin
forever on EOF re-printing their menus - a `timeout`-truncated
JIT-on-vs-off byte compare then "diverges" purely by SPEED; feed `q`
(they quit cleanly) before reading such a diff as real.

**N5 - FRAGMENT-LOCAL REGISTER CACHING.** Up to `MAX_CACHED` hot
int-scalar slots are pinned in `r12`-`r15` per fragment
(`pick_cached_slots`, jit.cpp):
loaded ONCE at the fragment head (the native back edge jumps AFTER the
entry loads, keeping them live across the loop), read/written straight
from the register (`read_slot`/`write_slot`/`load_operand` are all
cache-aware), and FLUSHED - the `t_int` singleton (held in rsi) to the
type word + the register to the payload, two stores - at EVERY exit/bail
(`flush_cache`, called by `exit_pc`). The accumulator/counter locals of
an int loop never touch memory in the steady state (`01_while` 0.098x
same-binary at the loop-bound extreme, scale 200 best-of-3). **SOUNDNESS - only resolved
LOCALS are cached, never TEMPS.** A slot qualifies iff every use in the
run is an int-scalar op AND it is a resolved local (`< slot_count`). A
TEMP (`>= slot_count`) is scratch the VM REUSES across run boundaries -
an int scratch inside one JIT run, a `foreach` general-array SNAPSHOT /
dict-iterator base / slice temp between runs - so the eager
entry-load/exit-flush (which assumes the register OWNS the slot for the
whole fragment) would overwrite a temp still LIVE as an array with the
int register + the `t_int` tag, corrupting the snapshot -> a later
`LoadElemValue` `InternalErrorEx`. A resolved local has a stable identity
and (counted only via proven-int ops) a stable int type. This corruption
was a `tests/nested_fuzz.py` find, NOT a `-rt` one (the aliasing needs a
specific temp-slot coincidence a hand-written test rarely hits), pinned
by two `jit:` regression tests. The classifier's operand extraction must
EXACTLY match the emitter's per-op layout: an earlier `IntAddStep`
mismatch counted a literal rhs VALUE as a slot index (a phantom that
could cache/corrupt whatever slot it collided with), and the
shift-by-register handler read its count raw - both fixed. The only raw
`slot_addr` reads left are on `bad()`-disqualified `LoadElem` base/index
slots (never cached). See plans/archived/native-aot.md.

**N6a - NATIVE MATH BUILTINS (`MathFnV`) + the REF-STORE FIX.** A typed
math-builtin (`sqrt`/`sin`/`cos`/`log`/`exp`/`pow`/... - `MathFnV`) is now
JIT-eligible: `sqrt`/float-cast lower to a pure SSE `sqrtsd`/`cvtsi2sd`, the
transcendentals to a libm CALL. **The call is a bare 5-byte `E8 rel32`, no
NOP padding** - patched after mmap to libm DIRECTLY (the anon `mmap(nullptr)`
lands within +-2GB of libm - MEASURED, and how the kernel lays anon maps
next to the loaded libs) or, out of range, to an out-of-line TRAMPOLINE in
the same buffer (`movabs rax,fn; jmp rax`, always reachable - also the
arm64-`BL`-veneer shape). **THE LOAD-BEARING FIX** (found because callgrind
showed the interpreter STILL running bench 40 under JIT-on): a ref-listed
scalar store (`store_dst`/`emit_float_store` - a reused temp that later
holds a string, so it CAN hold a reference) used to `cmp type == t_int/
t_float; jne BAIL`. That bailed on a TRIVIAL current value too (`none` on
iteration 1), so the fragment bailed at the first store and the interpreter
ran the WHOLE loop - the native code was UNUSED, and every "native builtins
are perf-neutral" measurement was interpreter-vs-interpreter. The fix
(**approach A**: a compile-time-proven native path, never a runtime bail):
test `type->t >= t_str` (a REAL reference - offset + `t_str` value probed
into `JitLayout`); a reference calls a noexcept C++ helper (`jit_put_int`/
`jit_put_float` - release + store, STAY native, cold/once-per-temp), a
trivial value takes the fast two-store. Effect (same-binary JIT off vs on):
the JIT was silently bailing across the suite - `08_func_call` **0.49x**,
`07_nested_loops` **0.58x**, `40_math_builtins` **0.72x** (my/py 5.6x ->
**7.7x**), `49_autoconst_fold`/`51_purefunc_fold` ~0.7x, broad -3-7%; no
regressions. This is the model for the whole JIT (approach A, see
plans/archived/native-aot.md): call the SAME C++ the interpreter calls
(arrays/dicts/
exceptions) from native, prove handling at COMPILE time, and DELETE the
interpreted original - no double copy, no runtime re-interpret. **Landed on
that model:** **`jit_raise`** - an OOB / negative-shift fragment stores a
`JitRaiseKind` to `g_vm_jit_raise` + exits to the op's pc, and `EnterNative`
raises via `vm_raise` (caret from the loc table, catchable) instead of
re-interpreting; and **delete-originals** - a run that is `op_fully_native`
(every op a non-throwing int op) AND single-entry has its interpreted ops
DROPPED from the bytecode (the remap maps every run pc to the EnterNative),
so `-vd` of a native int loop is just `enter.nat` (`-vdj` shows the fragment)
- a `.myv`-ready no-double-copy shape. A HELPER CALL clobbers r10/r11 (the N5
cache), so the active cache regs are pushed/popped around EVERY helper/
libm call (a nested_fuzz-found reg-clobber; a float/libm run caches nothing,
which masked it at first). The SLOTS BASE needs no such save - see the
register plan below.
**CONTAINER-STORE helper ops (LANDED):** `StoreElemInt`/`StoreElemFloat`
(`a[i] = v` / `a[i] OP= v`, a flat mutable int/bool/float array, LOCAL base)
are JIT-native - the fragment marshals base `LValue*` + (cache-aware) index +
value and CALLS `jit_store_elem_int/float` (vm.cpp), which run the SHARED
`vm_store_elem_*_body` (`ML_ALWAYS_INLINE`, the interpreter's EXACT store:
COW + bounds + the universal `vm_subscript_store` fallback). The store no
longer SPLITS the run - the whole matrix/sieve WRITE loop iterates natively.
**A raise is thrown LOC-LESS** (the helper runs the body with a NULL chunk),
caught into `g_vm_jit_exc` (an owned `RuntimeException`; complements
`g_vm_jit_raise`, a KIND a fragment signals itself), returned non-0; the
fragment `test eax; jnz` exits to the op's pc and `EnterNative` re-raises it,
stamping the caret from the LIVE chunk's loc table. A fragment CANNOT bake a
chunk pointer: `codegen_chunk` builds the chunk on the STACK and `std::move`s
it out AFTER `jit_compile_chunk`, so `&chunk` dangles (an ASan SEGV the
OOB-store regression test caught). Measured (same-binary before/after, both
JIT-on): 43_sieve 0.63-0.69x, 14_array_subscript 0.74-0.78x, 46_matrix 0.82x,
56_sieve_bool 0.79-0.90x (~1.3x on write benches); suite geomean 0.97-0.99x.
**#92 - the INLINE element-store tier + PREP (2026-08-02).** The
StoreElemInt/Float helper call above redid the whole managed model PER
STORE - type tag, storage kind, const + readonly, negative wrap, size()
twice, slice check, use_count(), the store, invalidate_hash() - measured
at **85 Ir to store one bool**, 66% of 43_sieve (the suite's worst bench,
27.2x C++). The guards + the raw store are now EMITTED INLINE for a plain
(aop invalid) int/bool store, the helper kept as the slow tier; every
guard DECLINES to it, so the negative wrap, OOB caret, COW, compound ops
and floats stay byte-identical. Measured: 43_sieve **-61.3%** Ir
(27.2x -> 10.5x C++), 14_array_subscript **-47.9%** (10.0x -> 5.2x);
88.9 Ir saved per store, the helper GONE from the sieve's profile (82.8%
native).
**THE GUARD IS `has_slices`, NOT `use_count() == 1`** - a MoveV-copied
dead temp keeps the refcount at 2 for essentially all array code, so a
sole-owner guard made the first version DEAD (caught by the emitted-code
counter `g_jit_store_fast`, the prove-it-ran rule). `clone_aliased_slices`
iterates `shobj->slices`, a no-op when no slice VIEWS exist - so
`SharedObject::has_slices` MIRRORS `!slices.empty()`, every set mutation
funneled through `slices_add`/`slices_del` (one iterator-erase in
`clone_aliased_slices` refreshes it itself), and the interpreted store
body ML_CHECKs `mirror == !slices.empty()` on every store.
**PREP - the COW clone as its OWN slow path, the store RESUMES native**
(maintainer's design): the two COW guards (slice base / live views) jump
to a stub calling `jit_store_elem_prep` - the interpreter's exact
normalization (clone_internal_vec for a slice base, else
clone_aliased_slices at the OVERLAPPED index only - cloning all slices
early is observable via `intptr`, which the COW tests pin) - then jump
BACK to the fast path's retry head. The interpreter pays the clone once
and stores raw ever after; now so does the emitted code. Prep runs only
AFTER the const/readonly guards (those throw WITHOUT cloning) and
bounds-checks in C++ before cloning (an OOB store must not detach). The
retry CANNOT spin: prep returns 0 only when `jit_cow_clean()` holds.
Sabotage-verified: clone-skipped-belt-intact degrades to correct-but-slow
(the helper's own clone takes over); readonly-guard-removed corrupts a
const array (value-divergence caught); COW-before-readonly ordering runs
prep on a must-throw store (the prep=0 assertion caught it - once the
test's slice used a `runtime()` index, since literal-bound slices of a
const FOLD at parse time and the first version was vacuous). `rsi` is
RESERVED (the fragment's t_int); the value rides `rdi`.
**#94 - FLOAT + BOOL PARITY (2026-08-02, maintainer-caught gap):** the
store tier shipped int/bool-only and the nested read int-only - the float
twins still paid the full helper per element. Now: StoreElemFloat takes
the same guards with an SSE tail (`movsd [rcx+r9*8], xmm0`, value loaded
via emit_float_load at the retry head since prep clobbers xmm0; prep is
kind-agnostic and shared), LoadElem2Float mirrors the int navigation with
float rows + `emit_float_store` (an INT row now takes the #95 cvtsi2sd
PROMOTE arm - it was a listed follow-up), and LoadElem2Int
gained the 1-byte BOOLS tail (byte count, movzx). `run_has_float` gained
LoadElem2Float so r8 = t_float is live in its runs. The float kind
guard's catching shape is MIXED rows (`[[1.0,2.0],[3,4]]` - the joined
elem type is float while one row's storage stays flat INT, so the READ is
LoadElem2Float over an int row); the pure-int-rows "promote" case reads
through LoadElem2INT and only promotes at the multiply - another
shape-eater, now listed. No suite bench exercises float element stores or
nested float reads (parity + reach, like prep); 46/43 neutral.
**#95 case 1 - COMPOUND element stores inline (2026-08-02):** `a[i] OP= v`
(incl. the `a[i]++` lowering) is a read-modify-write on the fast path -
same guards, then the element in RAX (hash byte invalidated FIRST so the
shobj register frees; past the guards nothing faults) with add/sub/imul
or cqo+idiv (quotient/remainder), floats via `xmm1 = elem OP xmm0`.
Refused at EMIT time: float `%=` (an fmod libm call), a LITERAL 0/-1
divisor (the helper runs the interpreter's exact C++ - the IntModRI
convention). RUNTIME divisor guards (a slot rhs, div/mod only) decline on
0 and -1, and are emitted BEFORE the prep jumps - a div-by-zero store
throws WITHOUT cloning, so prep must not run first; the float zero test
is `ucomisd` with a `jp` hop so a NaN divisor (unordered sets ZF) stays
fast. A compound on runtime BOOL storage is COMPILE-unreachable (the
store pins the base to array<int>), so the compound arm's ints-only kind
guard is defense in depth. The KIND checks moved BEFORE the prep jumps
for every arm (same invariant: prep must never clone a base the
interpreter would fault on without cloning). All four guard families
sabotage-verified; probing the bool shape exposed a PRE-EXISTING
tw-vs-VM divergence (a bool literal stored into an int-JOINED array -
tw threw, VM stored 1), since FIXED as #96 (maintainer-ruled VM-right):
**a bool WIDENS into flat numeric storage** - 0/1 into ints, 0.0/1.0
into floats, the promotion chain the decl/struct-field coerces already
followed - at all three engine-SHARED value-entry points
(flat_store_core, arr_append_fast, builtin_insert_arr; the JIT's inline
arms keep their narrow t_int/t_bool guards and decline a bool to the
helper, which widens). The reverse (an int into an array<bool>) stays a
TypeError - a narrowing. README documents the rule; a 5-mode entry pins
it, the tree-walker pass being the one that used to throw. No suite
bench compounds into an element (reach + parity, like prep).
**#95 case 2 - the NESTED store `a[i][j] = v` / `OP= v` inline
(emit_store_elem2_inline).** The read side (#93) had its tier; the store
paid the full helper. The fast shape: int k1/k2 SLOTS (boxed - type-tag
guarded against rsi/t_int), outer non-slice non-readonly GENERAL array
(the #93 navigation + byte-length bounds -> &row in rcx), row non-const
non-readonly flat int/bool/float, value FITTING the row's kind (int row:
t_int only - a bool does NOT fit, the interpreter's rule; bool row:
t_bool; float row: t_float or t_int, which PROMOTES via cvtsi2sd). The
row's COW pair routes to the SHARED jit_store_elem_prep on &row (rcx is
kept alive until the cow guards; the retry re-derives everything).
GUARD ORDER is the invariant: everything the interpreter throws on
WITHOUT cloning sits before the prep jumps - row const/ro, the row KIND
(a structs row's type error precedes any clone), the value-FIT
(flat_store_core checks `fits` before COW), a compound div/mod zero
divisor (apply_compound_op precedes the clone). Compound maps the Expr14
op to the base op and shares the RMW tails; the FLOAT arm refuses
div/mod at emit (a zero test on a maybe-promoted boxed value); a bool
row refuses any compound. StoreElem2V joined `run_has_float` (the float
arm reads r8) and the jit_op_nativized inline-tier acceptance (its
helper legitimately starves, like BinOpV's). Execution-proven by
`g_jit_store2_fast` with EXACT per-shape counts (the matrix builders
use single-level stores, so the counter attributes cleanly); sabotage-
verified: row has_slices (slice-oracle value + prep), the promote arm
(count 32->24), cow-before-fit and cow-before-divisor orderings (prep=0
fired), divisor guards (ASan FPE). The OUTER-readonly guard is SUBSUMED
by the row's for every constructible shape (deep const freezes every
level) - kept as defense in depth, recorded as unprovable in isolation.
N-level CHAIN stores (StoreElemChainV) stay helper-only by decision:
the walk is data-driven (a pooled step list), rare, and an inline loop
over steps buys little - revisit only if a profile ever names it.
**The codegen gap the float coverage EXPOSED (fixed):**
`compile_float_expr` had no arm for a DEFINITELY-int SUBEXPRESSION -
`as_float_operand` admits int LEAVES, but `row[j] = (j + 1) * 1.5`
carries an int CHAIN, and since the boxed catch-all leaves a
proven-float flat store to compile_float_stmt, the refusal escalated to
a NotLoweredEx on the WHOLE enclosing loop: a legal, ordinary program
REFUSED at compile time (the no-fail contract's other failure mode,
same class as the fold_lvalue_reads bug). The int subterm now compiles
via compile_int_expr into an int operand - every float reader
(read_float_operand, emit_float_load) promotes an int at runtime;
`definitely_int` gates it (a bool payload is not a float operand).
Pinned by a 5-mode differential entry that fails as NotLoweredEx with
the arm reverted (verified).
**#95 cases 3 + 4 - the PROMOTE arm and the SLICE read arms
(2026-08-02, completing the element-tier matrix):** LoadElem2Float's INT
row promotes inline (`cvtsi2sd xmm0, [rcx+r9*8]` - the mixed-rows shape
the helper used to serve), and SLICE bases read inline: the single-level
LoadElemInt/Float slice arm and the nested read's OUTER-slice and
ROW-slice arms (the outer arm rejoins the common row section, so
slice-of-slices composes). A slice's elements live at `data + (off+i)`
and its BOUNDS are the handle's u32 `len` - NOT the vector's size
(probed as `JitLayout::arr_off_off/arr_len_off` beside `slice_off`); a
negative index and bool/other-kind slices decline to the helper, as does
the promote-under-slice combination. Execution-proven by
`g_jit_elem_slice_fast` (single-level + row arms; the outer arm proves
via `g_jit_elem2_fast` on an only-sliced-outer shape) with EXACT
per-shape counts; sabotage-verified: the promote arm (16->0), the OFF
addition in both the single and row arms (parent-element value
divergence), and the LEN bound (a vector-size bound silently served
`sl[len]` - count 17 and a missed OOB). The #56 slow-tier test's proof
shape moved from the slice (now inline) to the NEGATIVE WRAP, which
still declines. Measured (callgrind Ir, `OPT=1 ASSERTS=0` both sides):
15_array_slice_readonly **-41.0%** (63.4M -> 37.4M - the per-element
helper call gone); 14/43/46/18/01/16 all +-0.01% neutral, and the
case-1/2 store restructure itself measured neutral-to-slightly-better
against pre-#95 (43 -0.05%). With these, the element-tier MATRIX is
COMPLETE: reads and stores, single and nested, int/bool/float, plain
and compound, slice bases, COW-prepped - every cell either inline or a
deliberate, documented helper decline (float `%=`, nested float
div/mod, chain stores, bool slices, promote-under-slice).
The **DICT store** `d[k] = v` / `d[k] OP= v` (LOCAL base) is the same shape -
`DictStore` -> `jit_dict_store` (vm.cpp), which runs the interpreter's exact
`vm_subscript_store`. The key/value are BOXED EvalValues in frame slots, so
the fragment just leas their addresses (no marshaling; an EvalValue is the
first `LValue` member); the base/key/value slots are DISQUALIFIED from N5
register caching (a cached int key - a counter used as `d[i]` - would leave
its slot stale). So a dict insert/update loop stays native instead of
splitting at the store. Measured (`23_dict_insert`, resolved with the
DETERMINISTIC callgrind I-count + a 25-run min+median after wall-clock noise
first mis-read it as 0%): **~6.5% wall / 12% fewer instructions** - dispatch
IS a real chunk of the dict tier, but the BIGGER headroom is the boxed-value/
alloc model (`my/cpp` ~5x in `bench/cpp/` - the N7 arc), not dispatch.
**`-vdj`:** decodes `push`/`pop`/`call`/`sqrtsd`/`nop`/`E8`-rel32/`lea`/`test`/
group-1 `cmp`/`sub imm`, and shows big `movabs` constants in hex (a `call rax`
had rendered as `dec rax`; a helper-call `lea rdi` had cascade-misdecoded as
`mov edi`).

**#55 STEP 1 — NATIVE `ReturnV` (plans/archived/native-call-impl.md).** A fully-native
LEAF body's `ReturnV` runs IN the fragment instead of the interpreter: the
fragment `flush_cache`s (so the result slot is in memory), then
`call jit_ret(res_slot); ret`. **`jit_ret`** (vm.cpp, `extern "C" noexcept`,
baked as a call target) reads the result from the CURRENT callee window via
`g_current_ctx->frame` (the running `vm_run_chunk`'s ctx - set + restored per
invocation by a `CtxGuard`; a builtin callback re-enters with the invoke ctx),
then either **pops the in-VM frame** (`vm_frame_leave` - writes the parent's dst
+ sets the `g_vm_resume_chunk/pc` globals) OR, at a **BOUNDARY** frame
(`g_vm_act->back_rec().boundary` - a `do_func_call`/callback callee), sets
`ctx.flow` (the callback contract, exactly the interpreted ReturnV's two paths).
It returns a resume **SENTINEL** (`static_cast<size_t>(-1)` in-VM, `-2` boundary
- a pc no remapped chunk pc can equal); `EnterNative`, on the sentinel, switches
`chunk`/`pc`/`code` to the parent, or `return`s to stop the invocation. A whole
body that is a single fully-native run ending in `ReturnV` is a
**`Chunk::native_leaf`** (fragment offset in `native_entry_off`; shown in `-vd`)
- a LEAF (makes no calls, C-stack-bounded) a caller fragment can `call`
directly (STEP 2). `ReturnV` is `jit_op_eligible` + `op_fully_native` (rets a
sentinel, never an interior pc, so its interpreted original is deletable) + in
`pick_cached_slots` (its result slot is an int use; a FLOAT result slot is
always disqualified by the float op that produced it, so it is never int-cached
here). Coverage: `g_jit_native_returns` (a `jit:` test asserts both the in-VM
and the boundary path ran).

**#55 STEP 2 — NATIVE `CallV` (plans/archived/native-call-impl.md).** A function->
function direct call to a `native_leaf` runs as a native `call` from the caller
fragment, not an interpreted CallV. **2.0 (ordering foundation):**
`jit_compile_chunk` moved OUT of `codegen_chunk` for the precompile - `codegen`
sets the `native_leaf` FLAG (`jit_chunk_is_native_leaf`, from ops) and takes a
`jit` param, and `vm_precompile_all` codegens ALL bodies THEN jits ALL, so every
callee's flag exists before any caller is jit'd (a caller bakes the callee
`FuncDescriptor*` and loads its `native.base+native_entry_off` at RUNTIME - the
flag is the only compile-time need). **2.1 (the call):** a `JitCtx` (slot->desc
map, `global_slot_reassigned`, the chunk's own `caller_desc`) is threaded into
`jit_compile_chunk`; `callv_native_ok` is the COMPILE-TIME gate (a plain CallV,
write-once global slot, callee `native_leaf`, function caller); `op_run_eligible`
folds it into run-building (since the MIN_RUN removal EVERY run forms, so the
old call-bearing-run exemption — `run_has_native_call` — is gone; the boxed
arg-setup MoveVs still split a call loop into short runs, each now a
fragment). The emit: call **`jit_call_setup`**
(vm.cpp - resolves the callee FuncObject from its global slot, `vm_frame_setup`
with `ret_chunk = caller_desc->vm_chunk`, `ret_pc = pc+1`; catches
StackOverflow/bind throw -> `g_vm_jit_exc` + null return); `test/jnz` (null ->
`exit_pc` so EnterNative re-raises); load `callee->vm_chunk->native.base +
native_entry_off` and `call` it directly (the callee's native ReturnV/jit_ret
pops + writes OUR dst + rets a sentinel we IGNORE - a native_leaf never bails
post-setup); epilogue. A call run is NOT N5-cached (`pick_cached_slots` -> {}),
so args are in memory and no reload is needed; a call-bearing fragment is
non-leaf (never native-CALLED), so its StackOverflow exit always returns to
EnterNative. Layout offsets (`FuncDescriptor::vm_chunk`, `Chunk::native.base`,
`Chunk::native_entry_off`) are probed in vm.cpp. C-stack is bounded (F2 v1):
only LEAF callees are native-called, so a native call adds one fixed C frame; a
recursive/non-leaf callee isn't `native_leaf` -> interpreted CallV. Coverage:
`g_jit_native_calls` (a `jit:` test). Measured ~9% on an all-calls microbench
(the win is dispatch removal; `vm_frame_setup` is shared with the interpreted
path). **`-vd`/`-vdj` are FAITHFUL:** `disassemble_program` (disasm.cpp)
replicates the precompile's two-pass + `JitCtx` on throwaway chunks (pointing
each `desc->vm_chunk` at its local chunk for the gate, save/restored after), so
a native call shows as `enter.nat` at the caller's call site and `-vdj` decodes
the full sequence (`call jit_call_setup` / `test`+`jne` / the
`vm_chunk`->`native.base`+`entry` loads / `call rcx` / epilogue). **v1 non-native
cases (always a correct interpreted fallback):** a call FROM MAIN (main has no
stable descriptor for the record's ret_chunk). `CachedCallV` is excluded too,
but MOOT (its callee is a cacheable RECURSIVE func, never a `native_leaf`). A
CONST-ARG call is NOT a gap: a pure callee folds at compile time (optimal - no
call), an impure caller / runtime arg still native-calls, and a native_leaf
rarely specializes to a clone (const-arg propagation on a small int body doesn't
shrink it) - all verified.

## G5 (2026-08-06): `len(str)` emitted INLINE - one load, no call

`StrLen` was a helper call at ~48 Ir. The split, by the file each inlined
frame came from, is the interesting part:

    evalvalue.h   21   the boxed LValue::put of the RESULT
    vm.cpp        14   the helper body + the call frame
    eval.h         8   Frame::at x2
    sharedstr.h    5   the actual string access
    basic_string.h 1

So the string access was never the cost - the CALL and the BOXED STORE
were. The inline tier removes both: one zero-extending 4-byte load of the
window length, then the ordinary ref-aware `store_dst` (which C3/C5 can
then elide like any other scalar store).

**What unblocked it.** An inline read needs the length in a FIELD, and
before THE WINDOW MODEL (#123, sharedstr.h) `size()` was
`slice ? len : obj->s.size()` - a dependent load through `obj` that no
emitter can do without baking libstdc++'s std::string layout, which the
co-located-probe pattern cannot supply (there is no portable way to take
the address of a std::string's size field). With `len` authoritative for
every SharedStr, the emit is one instruction. **The correctness fix and
the optimization were the same change**, which is why G5 sat blocked
until #123 landed.

**No type guard**, deliberately: codegen emits `StrLen` only where
`vm_len_kind` proved the argument is a string - the same stamp that picks
`ArrLen` - exactly as ArrLen's helper trusts its proven array.

The offset comes from `SharedStr::jit_probe()`, a co-located probe reading
the real member (the field is private, hence the accessor), so `jit.cpp`
cannot grow a second copy of the layout that drifts.

**The helper is DELETED** (delete-originals): the interpreter has its own
`VM_CASE(StrLen)`, so `jit_str_len` became unreachable. Its
`ML_JIT_OP_RAN` slot went with it, so `StrLen` is out of the nativized-ops
coverage table; the execution proof is the emit-side `g_jit_strlen_fast`,
asserted by `jit_len_ord`.

**A VACUOUS SABOTAGE, worth recording.** The first attempt swapped
`str_len_off` for `arr_len_off` - and the suite stayed GREEN, because
`SharedStr` and `SharedArrayObj` have the same head layout
(`intrusive_ptr` + `off` + `len` + `slice`), so the two offsets are
EQUAL. The discriminating sabotage is `arr_off_off` (the window OFFSET,
a different field), which fails 3 tests. `jit_len_ord` also gained a
SLICE case - a window with a non-zero `off` is precisely where reading
`len` and reading `off` diverge.

Measured (`OPT=1 ASSERTS=0`, callgrind Ir, both sides built this session):

    75_indexed_unpack     2414M -> 2004M   -16.98%
    29_str_slice_readonly 56.8M -> 47.8M   -15.84%
    30_str_index_iterate                    -0.01%   (its len() is the
                                                      for-range bound -
                                                      evaluated ONCE)
    31_split_join / 47_wordcount / 41_str_int_conv    +-0.00%

75_indexed_unpack was the WORST bench against C++ (20.03x) and calls
`len()` twice per row, which is where the whole 17% sits.

## G4 (2026-08-06): the CHECKED `a[i].f` struct-field read

`LoadStructFieldInt/Float` already read a scalar straight from flat
struct-array bytes, but it was emitted ONLY by `try_sfe_field`, whose gate
is a struct-FOREACH loop var. A SUBSCRIPT base fell all the way back to a
boxed subscript - materialising a whole `StructObject` per read - plus a
boxed member read, which is why `row[0].x + row[1].y` cost ~490 Ir.

**It is not a gate widening.** `vm_struct_field_int` is UNCHECKED by
construction ("the codegen proved it flat-struct + idx in range - the
counted loop - so no checks"), and a subscript proves NEITHER: a flat
`array<PodStruct>` AUTO-PROMOTES to general storage on any cold op
(insert/sort/map via `get_vec()`), and the index is arbitrary. So the new
form wraps a negative index, bounds-checks, and serves BOTH storages - the
general arm reading the same FIELD INDEX out of the boxed StructObject,
which is valid because the element is a POD struct either way.

`struct_checked` is `opflags` bit 7 - the last free bit in a byte that is
ALREADY serialized - so it rides the same opcode: no ordinal moves and no
`.myv` version bump.

**CARET-NEUTRAL by design.** The op stamps the SUBSCRIPT node, which is the
span the boxed pair reported, so the default engine's OOB message does not
move. The tree-walker reports the MemberExpr's span here instead, a
PRE-EXISTING divergence this change deliberately neither widens nor
resolves - task #125, found while scoping this.

### Two traps, one of them a bug I introduced

**(1) THE NO-FAULT FUSION ATE IT.** #9 F-C fuses `LoadStructFieldInt t;
IntBin(+) dst = other + t` into `StructFieldAddInt`, sound only because the
foreach form cannot fault - it even drops the caret (`node_idx = -1`) - and
`StructFieldAddInt`'s helper is the UNCHECKED reader. The checked form
flowed straight into it, and ASan reported a **heap-buffer-overflow** on
`s += a[i].x + a[j].y` with `j` out of range the FIRST time the gate let it
through. The fusion now excludes `struct_checked()`. General shape worth
remembering: **when you make an existing opcode faultable, re-audit every
peephole that fused it BECAUSE it could not fault.**

**(2) A FAULTABLE OP NEEDS A LOC ENTRY, and the switch could not say so.**
`extract_locs` is per-OPCODE, and `LoadStructField*` was classified
no-fault, so it recorded no caret - the interpreted VM threw with NO
location at all (no file, no line, no caret). Since both forms share an
opcode, the distinction cannot be a `case` label: the loc is recorded by an
explicit pre-switch check on `struct_checked()`.

Also worth noting: the declaration of the new helper went inside jit.h's
`#ifdef TESTS` block by mistake. The DEBUG lane built fine and the release
lane did not - the non-TESTS build gap, the same family as the non-JIT
platform gap.

### Measured (`OPT=1 ASSERTS=0`, callgrind Ir, both sides this session)

    77_struct_array_lit   1571M -> 797M   -49.26%
    65_struct_field_sum                    +0.00%   (foreach form, untouched)
    58_structs                             +0.00%

**-49% is far more than the ~16% the scoping predicted**, and the estimate
was wrong in an instructive way: it counted only the two boxed member reads.
Making them typed also turned the CONSUMING arithmetic typed - the loop's
`bin.v` + `compound.v` became `i.bin` - so the win is the whole expression,
not the reads alone. Reach is still one program: `member.v` occurs only in
77 across bench/ + samples/.

## G3 (2026-08-06): skip the IDENTITY field coercion when building a struct

After G4, 77_struct_array_lit's remaining cost is the BUILD - 68% of the
bench, ~1089 Ir per iteration. The per-element ALLOCATION is already gone
(top-10 #5's dst-reuse overwrites the previous iteration's bytes), so what
was left is per-FIELD:

    vm_make_struct_array_op   737
    coerce_struct_field       196
    pod_store_field            80
    coerce_to_decl_type        76

`coerce_struct_field` takes its `EvalValue` **BY VALUE** - a 32-byte
non-trivial type, so a copy in and a copy out - and then calls
`coerce_to_decl_type`, which does the same again. That is the
by-value-parameter disease `vm_frame_leave` (#82) and lever 1 were cured
of, running once per FIELD of every struct built. For a field that already
holds its declared kind the whole thing is identity.

`field_exact_scalar` (structtype.h) answers "is this already exactly the
field's scalar kind", and the two build loops then call `pod_store_field`
(which takes a const reference) directly. **EXACT match only**: every real
widening - bool -> int, bool/int -> float - still goes through the real
coercion, as do `none` into an opt field and every non-scalar kind.

**THE TEST SHAPE IS THE WHOLE DIFFICULTY.** The edit is in
`vm_make_struct_array_op`'s DST-REUSE arm, which only runs once the slot
ALREADY holds a matching array - i.e. from the second iteration of a
LOOP-CARRIED literal. A one-shot `var a = [W(..)]` takes
`vm_make_struct_array` instead and never reaches it, so a non-loop test is
vacuous; the first one written here was exactly that and the sabotage
sailed through it.

And the sabotage's failure is **VM-ONLY**: the tree-walker does not use
this op at all, so the headline `Tests passed: N/N` (the tree-walker pass)
stays green and only the four Differential lines flip - plus the exit code.
Read those lines, not the headline, when the change is VM-side.

Sabotage watched failing: accepting a bool as an exact int (so a real
widening gets skipped and the raw payload is stored) -> 1531/1532 in all
four differential modes, exit 1.

Measured (`OPT=1 ASSERTS=0`, callgrind Ir):

    77_struct_array_lit   797M -> 602M   -24.52%
    64_struct_create / 58_structs / 65_struct_field_sum    +0.00%

Cumulative for 77 across G4 + G3: **1571M -> 602M, -61.7%**.

**SIBLINGS NOT TAKEN:** `vm_make_struct_array` (the FRESH-build path, what a
non-loop-carried literal uses) and `construct_struct_from_values`
(StructCtorV) have the same per-field identity coercion. The latter got the
same treatment here; the fresh array path did not, and is why
64_struct_create is flat.

## G1 increment 1 (2026-08-07): the inline push's GUARD phase

Four precomputes in `emit_sync_push_native`, each replacing a per-call
recomputation with a quantity that was already derivable. No new proof
obligation, no new flag whose staleness could bite.

1. **`Chunk::plain_frame` for the three callee-count tests.** The push used
   to compare `n_dict_iters`, `n_dyn_iters` and `n_trys` separately (six
   instructions); that flag already means exactly "owns no per-frame side
   state", and the POP side has trusted it since 2026-08-01. Same proof, one
   byte test, and now both ends of the protocol read one flag. **-4**
2. **`VmStackSeg::cap_slots`.** The segment fit test compared BYTE extents:
   `imul` the slot sum by 48, then rebuild the vector's length from its two
   pointers - which needed rcx spilled as a second scratch. `slots` is sized
   once at construction and never resized (live windows hold raw pointers
   into it), so its capacity is a constant; the test is now one compare in
   slot units. rax is still spilled - every register is live here - and
   `pop` leaves the flags alone. **-5**
3. **The two iterator watermark bases in ONE qword move.** Both are adjacent
   u32 pairs. The emitter CHECKS the adjacency from the probed layout rather
   than assuming it, and falls back to the two-move form. **-2**
4. **The arg-zeroing `xor r11d, r11d` is emitted only when there are args.**
   **-1**

Measured (callgrind Ir, `OPT=1 ASSERTS=0`): the probe's caller fragment
159 -> 147 Ir per call (**-7.55%**, exactly the 12 instructions), its callee
fragment byte-flat (the pop is untouched); 10_recursion_deep **-3.99%**,
11_closure_counter **-2.89%**, 63_closures **-2.29%**,
76_funcval_dispatch **-1.22%**, 09_fib_recursive -0.14%. Ten unrelated
benches flat to the last digit - no dispatch layout tax.

### RULE THIS EARNS: the emitted push serves REPEAT calls, not DEEP ones

Instrumenting `g_jit_sync_inline` while looking for a test shape produced a
reach measurement worth more than the increment:

    a closure called 2,000,000 times in a loop   2,000,000 inline pushes
    down(n) with an int param, 6000 deep                 0
    down(dyn n), 8000 deep                              0
    zero-arg self-recursion, 3000 deep                  0

Two independent gates: **`fast_bind` is FALSE for any callee with an
`int`/`float`-declared param** (such a param needs a coercion, not a copy),
and the **record-reuse guard `rec_n != recs_high`** declines a
monotonically-deepening chain - a first descent always grows `records`, so
it always takes the C++ tier. Before attributing a call-protocol measurement
to this emitted path, CHECK THE COUNTER; the shapes it does not serve are
not exotic.

### The sabotage that needed a purpose-built shape

A half-copy of the watermark pair is invisible to every ordinary program.
The slice is sized at FRAME push from the chunk's own count, so **within one
frame the watermark never moves** - the stale value and the live value are
equal, and no differential can tell them apart. The catching shape needs the
SAME record slot reused under two DIFFERENT correct bases: `A` owns a dyn
foreach (dyiters_n is higher for as long as it runs), `B` owns none, both
calling one zero-arg closure at the same depth. Then the half-copy aborts on
`jit_ret_audit`'s `dyiters_n == rec.dyiter_base`.

Three shape-eaters had to be defeated for the callee to reach the push at
all, and they are why the test looks contrived: it must be a **closure** (a
named function is folded or inlined away), take **no argument** (an int
param kills `fast_bind`), and be called from a **loop** (the first call
declines on record reuse).

Sabotages watched failing: removing the `plain_frame` gate -> `-rt` exit 134
on `jit_dyiter`'s slice bound; `cap_slots` inflated in C++ -> the ML_VM_CHECK
at `push_window`; the fit test pointed at `seg_top` instead of `seg_cap` (it
then always declines - silently correct, only slower) -> the EXISTING
coverage test, `jit_sync_inline_call: inline path DID NOT RUN`; the half-copy
-> the new test above.

## G1 increment 2 (2026-08-07): the two-entry callee cache

One `Chunk::CalleeCache` cell per emitted inline call site. A hit skips the
five callee-PROPERTY guards - `fast_bind`, the arity match, a compiled chunk,
a native sync entry, `plain_frame` - because every one of them is fixed once
the descriptor and its chunk exist (`vm_chunk` write-once under
`vm_chunk_tried`, `fast_bind` set with it, `params` frozen at `sync_params`,
`sync_entry_off`/`plain_frame` set by the compiling tier). 13 instructions
become 4 (entry 0) or 6 (entry 1).

**Key on the `FuncDescriptor *`, never the `FuncObject *`.** The object key
would be one instruction cheaper - it subsumes the type check - and it is the
stale-pointer identity bug: a FuncObject is refcounted, so a later closure at
a freed one's address would hit an entry describing a different function. A
descriptor is program-lifetime.

### RULE: a one-entry inline cache regresses the shape it was written for

The one-entry version measured 10 -3.40%, 11 -2.23%, 63 -1.85% and
**76_funcval_dispatch +0.56%** - and 76 (`ops[i % 2]`) is the bench the whole
arc is named after. A second entry costs a monomorphic site NOTHING (entry 0
is tested first, same three instructions) and takes 76 to **-0.90%**. If you
add an inline cache here, measure the ALTERNATING shape before believing the
monomorphic one.

**The shift is MRU and it is load-bearing**: new callee to entry 0, old one
down to entry 1. Filling entry 1 first leaves a monomorphic callee behind a
compare it can never pass - +2 instructions per call forever, every answer
still correct. The probe read it as 138 -> 140 Ir/call.

### The two hit arms are counted SEPARATELY, and that is why the order is
### testable

`g_jit_callee_cache` (entry 0) and `g_jit_callee_cache2` (entry 1), TESTS
builds only - a release patches both jumps to one address and emits neither.
One counter cannot see WHICH entry answered, and a backwards shift changes no
result, so a single counter would have left the MRU order as a number in a
measurement log. Sabotages watched failing: collapse to one entry ->
`the SECOND entry is dead (0)`; always hit (cmp rax,rax) -> `-rt` exit 134 on
`jit_dyiter`'s slice bound; reverse the shift -> `hit arm DID NOT RUN (0)`.

### THE VACUOUS-TEST TRAP, one level up: the eater was the KEY

The first polymorphic test built its two callees from ONE lambda decl. Two
closures of one decl SHARE a FuncDescriptor, and the cache keys on the
descriptor - so the "polymorphic" site was monomorphic and the second entry
was never exercised. Add to the shape-eater list: it is not only an optimizer
pass that can eat a test shape, it is any identity the mechanism keys on.

Measured (callgrind Ir, `OPT=1 ASSERTS=0`), on top of increment 1: probe
caller fragment 147 -> 138 Ir/call (**-6.12%**); 10_recursion_deep -3.40%,
11_closure_counter -2.23%, 63_closures -1.85%, 76_funcval_dispatch -0.90%,
09_fib -0.09%; nine unrelated benches flat.

## G1 increment 3 (2026-08-07): the coercing bind goes inline

**An `int`/`float`-declared parameter used to decline the ENTIRE inline push
to the C++ tier, and that measured 1.61x** on the same closure called two
million times with only the annotation changed (742.6M Ir for `dyn k`,
1194.6M for `int k`; `jit_call_sync_core` was 180M of the difference). So the
language's own encouragement - annotate your parameters - made every call to
such a function 61% more expensive.

`coerce_to_decl_type` is the IDENTITY when the value already has the declared
type, which is what the inferencer produces. `FuncDescriptor::bind_req` names
that requirement per parameter (`t_int`/`t_float`, null for none), populated
only when `fast_bind` is false; the emitted push tests `fast_bind` as before,
and on the false arm requires each argument to already hold its parameter's
type before falling into the SAME copy loop. A widening (bool into int, int
into float), a `none`, or a wrong type still declines - correct, just slower,
because the conversion would have to happen in the copy loop, which runs
AFTER the record is pushed and so can no longer decline.

The coercing callee is deliberately NOT cached: increment 2's cache
re-establishes DESCRIPTOR properties, and this one depends on ARGUMENT VALUES.

**One derivation, four callers.** `fast_bind` was recomputed inline at three
sites and now something else must agree with it - the audit-table trap in
miniature - so `compute_bind_flags` (eval.cpp) is the ONE reader of
`decl_type`. The fourth caller is the .myv reader: `bind_req` is derived and
unstored, and recomputing `fast_bind` beside it makes the stored byte a free
ML_CHECKed cross-check of the param round trip.

### RULE: a guard that is TOO STRICT is invisible to every correctness net

Swapping the required types (an `int` param demanding `t_float`) makes every
call decline - which is CORRECT, so the 5-mode differential, the corpus, the
fuzzer and the lever matrix are all green on it. Only `g_jit_bind_coerce`
sees it: `is ZERO after the whole suite - the lever is dead or untested`.
Whenever a new tier's failure mode is "declines more than it should", the
emitted-code counter is not a nicety, it is the ONLY net.

The other sabotage - dropping the per-argument check - fails the JIT-ON
differential modes (1534/1535) while the headline stays 1753/1753, the
documented VM-only shape.

Measured: q_int 1194.6M -> 602.6M (**-49.56%**, 597 -> 301 Ir per call), now
FASTER than the `dyn` twin; q_dyn and the zero-arg probe byte-flat.
**bench/ was flat on all 14 call-heavy and control benches** - a finding
about bench/, not the change: no benchmark annotated a parameter on a
call-heavy path, so the suite could not see a 1.61x that any annotated
program pays. FIXED the same day by `78_typed_param_call`, which reads
cur/base 0.60x (1.67x faster) against the pre-G1 binary.

**SIBLING TAKEN the same day - see the next entry.** The note below is kept
because it states the constraint that increment had to design around.

**SIBLING NOT TAKEN**: a WIDENING argument (`func f(float x)` called as
`f(i)` with an int `i`) still declines every call - `g_jit_bind_coerce` reads
0 for a loop of exactly that, against 100 for the exact-type twin. A widening
is TOTAL (it cannot throw) so it could be inlined, but the conversion has to
live in the COPY LOOP, which runs after the record is pushed; the guard would
then have to accept a SET of types per parameter and the copy loop would need
a conversion arm - two emitted pieces instead of one. The only genuinely
un-inlinable residue is the NARROWING throw (a float into an `int` param),
which raises before the frame exists.

## G1 increment 4 (2026-08-07): the widening argument converts in the
## caller's own temp

A conversion in the COPY LOOP would happen after the record is pushed, where
a decline is no longer possible - that is why increment 3 left widenings
declining. The way out: the widenings are TOTAL (bool -> int is a pure RETAG,
since a bool's payload is already the int 0/1; int/bool -> float is one
`cvtsi2sd`), so nothing about them needs to happen late. They now run EARLY,
in the guard phase's coercing arm, writing the caller's own argument temp;
the copy loop then finds an exact value and is untouched.

**Why writing that temp is sound**: `emit_args_range` gives every argument a
FRESH temp compiled immediately before the call, so nothing reads it
afterwards - and the value written is exactly what `coerce_to_decl_type`
would have produced at bind, so a LATER guard declining to the C++ tier still
binds the right thing (its own coercion is then the identity). Only the
NARROWING and a non-numeric remain declines: they must RAISE, which is
precisely what cannot happen once the frame exists.

### THE REGISTER TRAP: `rax` is the DESCRIPTOR through the whole guard phase

The first version used `rax` as the scratch for the argument's kind byte and
**every call died on `Frame::at`'s bounds assert** - the frame-size read and
the cache-hit arm both still need the descriptor. The arm uses `rsi` (it
carries the fragment's pinned int tag and is overwritten with `total` a few
instructions later - dead-then-redefined) and `r10` (not live until the
record fill). Same family as the ABI traps above: an implicit register
contract, violated by an addition to a long function.

### The counter has to be an extra_check, not a `tests` entry

`jit_counter_coverage` runs during the TREE-WALKER pass, before the
differential modes have executed any native code - so a source-table test
cannot feed a `g_jit_*` counter in time. The first attempt was exactly that
and the coverage assertion reported the lever dead. Anything that must PROVE
an emitted path ran needs an extra_check that drives `vm_execute` itself.

Measured: 4M widening calls 3182.0M -> 1978.0M (**-37.84%**, 796 -> 495 Ir
per call); the exact, dyn and zero-arg probes byte-flat, fourteen benches
flat. Sabotages watched failing: skip the int -> float conversion -> the
extra_check AND the differential (1534/1535); accept a narrowing -> the two
JIT-ON differential modes (1534/1535) while the headline stays green.

## G1 increment 5 (2026-08-07): the coercing callee joins the callee cache

Increment 3 left a coercing callee re-running the five descriptor-property
guards on EVERY call, because increment 2's cache could not hold it: a plain
hit goes straight to the push, a coercing one must still run the
per-argument checks (they are about ARGUMENT VALUES, which no descriptor
match re-establishes).

`Chunk::CalleeCache` gains a THIRD entry tested only after both plain entries
miss - so a fast_bind site pays for it never on a hit, once on a miss. A
coercing hit skips the arity compare, the chunk load+test, the sync-entry
test, plain_frame and the fast_bind test, re-derives only the callee chunk,
and falls into the per-argument checks.

Predicted 10 instructions, measured exactly 10: q_int 301 -> 291 Ir/call
(**-3.32%**), widen2 495 -> 485 (**-2.02%**); the dyn and zero-arg probes
byte-flat, twelve benches flat.

Sabotage watched failing: point the third compare at a register that cannot
match -> the widening extra_check's own assertion (`the coercing callee is
NOT CACHED (0)`) AND the coverage sweep (`g_jit_coerce_cached is ZERO`),
1752/1754. Both are counter-based of necessity: a cache that never hits is
CORRECT, so no differential, corpus, fuzzer or lever configuration sees it.

## G1 increment 6 (2026-08-07): the callee-resolution `defined` probe

The global-slot call form resolved its callee in nine instructions, three of
them a `defined` probe - a load of a SECOND vector's data pointer and a byte
test on a different cache line from the slot it precedes.

It is redundant, and making that TRUE rather than likely is the substance:
**`GlobalFuncTable::define` / `put_defined` are now the only two ways to
write a global slot**, each marking `defined` in the same statement as the
store, so an unbound slot still holds the default `none` and the type test
declines it. The C++ tier then raises UndefinedVariableEx with its caret
exactly as before. Six writers routed through them; the emitted StoreGlobalV
writes both natively and is unchanged. `jit_call_sync` - which every decline
and every FIRST call at a site reaches - ML_VM_CHECKs the invariant.

Measured: 10_recursion_deep **-1.17%**, 45_gcd -0.36%, 63_closures -0.25%,
09_fib -0.08%; everything else flat. Ir understates it - the call path also
stops touching a second cache line.

### RULE: the G1 probes measure the VALUE call form, bench/ the GLOBAL one

`q_proto`, `q_int` and `widen2` all read -0.00% here and briefly looked like
a build that had not recompiled. They had; the emitted code is provably three
instructions shorter. Those probes call a closure held in a TOP-LEVEL VAR -
which main reads itself, so it is a main-frame LOCAL and the call is
CallValueV, a form that never had a `defined` probe. A named top-level
function called from inside another function is the global-slot form, and it
lives in the benches. **Check which form a measurement exercises before
concluding a call-protocol change did nothing.**

Sabotage watched failing: drop the mark from `define()` -> 1359/1754 plus
both JIT-ON differential modes. The removed guard itself is unfalsifiable at
the emitted site (a slot can only be unbound on a site's FIRST executions,
which decline on record reuse anyway) - which is exactly why the safety
argument lives in the store/mark pairing, which is falsifiable.

## G1 reach (2026-08-09): `MYLANG_JITSTATS=1`, and the PREPARATION's reach
## claim is now STALE

The arc kept needing one number and kept not having it: **which tier does a
call on a REAL program actually take?** Every JIT counter exists and every
JIT test asserts on one, but they were readable only from inside `-rt`, so
"does ordinary recursion reach the inline push, or decline to the C++ tier?"
could be asked of a hand-written probe and of nothing else. That is how the
G1 PREPARATION came to quote a per-call cost for a path it then measured a
large fraction of real calls never taking.

`jit_stats_report()` (jit.cpp, called from the driver) prints the
emitted-code counters after a script run when `MYLANG_JITSTATS=1`. A
**TESTS=1 build only** - the bumps are `#ifdef TESTS` emitted code, so a
release has no counters and printing zeros would be a lie; it says so
instead. Inert otherwise: one `getenv` at exit.

### What it says, `TESTS=1 OPT=1 ASSERTS=0`, scale 1

    bench                sync_inline  cache  cache2  coerce_cached  ret_inline
    10_recursion_deep      1,352,549  1,352,997    -            -   1,353,001
    45_gcd                   149,998    149,998    -            -     150,000
    76_funcval_dispatch      999,999    499,999  499,999        -   1,000,000
    78_typed_param_call    2,000,001          -        -  1,999,998   1,999,999
    09_fib_recursive          10,687     10,689    -            -           0

**The PREPARATION's reach finding no longer holds.** It recorded

    down(n) with an int param      0 inline pushes
    zero-arg self-recursion        0

and concluded the emitted push "serves the repeat-call shape, not the
descend-deeper shape". Increments 3-5 closed both gates: 10_recursion_deep
and 45_gcd now take the inline push on **essentially every call**, with a
~100% callee-cache hit rate and an inline return to match. So the arc's
per-call savings DO reach ordinary recursion, and the sentence saying they
do not should not be quoted again.

Two details the table settles, both by design rather than gaps:
- **76** splits 50/50 across the two cache entries - the strict `add_op`/
  `sub_op` alternation with MRU promotion, hitting on every call after the
  first. That is exactly what the second entry was added for.
- **78** hits neither entry: a coercing callee lives in the THIRD entry
  (increment 5), and `coerce_cached` accounts for all 2M calls.

### The one shape with NO inline return, and why it is not a bug

**09_fib_recursive: 10,687 inline pushes, ZERO inline returns.** The
inline pop declines when the record carries a `cache_key` or the frame has
a live `pure_cache` - both documented gates - and `fib` is a
`cache_results` function, so every one of its returns is the slow tier by
construction. Not worth attacking: the AST recursion-unroll plus the
per-frame pure-call cache already removed **99.4%** of fib's calls
(~1.7M -> 10,687), so the return path there is no longer hot.

## G1 fork REACH (2026-08-09): what a no-record tier would actually cover

The fork asks what a callee that needs NO VM frame record looks like -
"a leaf, plain-frame, fixed-arity callee whose params are scalars". Before
designing one, measure how many real calls that describes. A probe
(`norec_classify`, vm.cpp, `#ifdef TESTS`) classifies every in-VM call
against the four gates in order; `MYLANG_JITSTATS=1` reports it.

It sits on the **C++ push path**, so it is read with the **JIT OFF** - and
that is sound for this question: the JIT changes how a push is emitted,
never WHICH callee a call reaches, so the shape distribution is identical.
`leaf` is computed from the BYTECODE (any call-like opcode in the body),
not from `Chunk::native_leaf`, which is a JIT product and is false on the
very run the probe needs.

### The corpus, 6,953,702 in-VM calls (bench/ + samples/, scale 1)

    program                  calls    plain     leaf    arity   scalar
    78_typed_param_call    2000002  2000002  2000002  2000002  2000002
    10_recursion_deep      1353000  1353000        0        0        0
    11_closure_counter     1000001  1000001  1000001  1000001  1000001
    63_closures            1000000  1000000  1000000  1000000   800000
    76_funcval_dispatch    1000000  1000000  1000000  1000000        0
    69_exc_crossframe       340000   340000        0        0        0
    45_gcd                  149999   149999   149999   149999   149999
    44_primes_sqrt           99998    99998    99998    99998    99998
    09_fib_recursive         10696    10696        0        0        0
    ------------------------------------------------------------------
    share of all calls                100.0%    75.5%    75.5%    58.2%

**plain_frame is free** - every call in the corpus already qualifies.
**Fixed arity is free** - it excludes nothing the leaf gate had not.
The two that cost are `leaf` (-24.5%) and `scalar params` (-17.3%).

### The finding that decides the fork's shape

**The leaf gate excludes RECURSION entirely, and recursion is where the
protocol hurts most.** A self-recursive callee is never a leaf, by
definition - so 10_recursion_deep (1.35M calls, 19% of the corpus) and
09_fib_recursive contribute ZERO, as does 69_exc_crossframe. Those are
precisely the shapes that pay the protocol per level and have nothing else
to amortise it against.

What the strict tier WOULD cover is the closure/dispatch family (11, 63,
76) plus the typed-param and iterative-helper shapes (78, 45, 44). Note
45_gcd counts as a leaf legitimately - it is written ITERATIVELY - and 76
fails only the scalar gate, because `add_op(st, x)` takes an array.

**Honest sizing: 58.2% of executed calls, or 41.4% excluding
78_typed_param_call** - a bench this arc added for itself, and 2M of the
4.05M qualifying calls. Per PROGRAM rather than per call: 5 of the 13
programs that make any call are at ~100%, one at 80%, and seven at 0%.

### So the design question is not the one the fork asked

"Leaf" is not a requirement of the idea, it is the easy version of it: it
guarantees no deeper frame can need our record. Relaxing it - a non-leaf
callee that still skips the record, reconstructing it only when a deeper
frame, an exception or a backtrace asks - is what takes reach from 58% to
the 75-100% band AND is the only version that touches recursion at all.
That is a materially harder design (the reconstruction needs the caller's
resume pc and dst, which is most of what the record holds), and it is the
question worth putting to the maintainer, rather than "should we build the
leaf tier".

## G1 no-record tier STEP 1 (2026-08-09): the shadow-verified side table

plans/archived/g1-no-record-tier.md steps 1 + nets 1/1b/5/6, zero behaviour: one
NorecSite per emitted sync-call site ({caller, call_pc, dst, site_loc, op,
two ret-address offsets - the M5a switch emits `call rdx` TWICE}), filled
to absolutes where call_relocs are patched, registered in ret-addr -> site
and range -> chunk maps (jit_norec_register; container chunks register
their RANGE too - they can be sync CALLEES). TESTS builds store the site
pointer into the record (rec_norec_site; push_window nulls it on every C++
push) and call jit_norec_push_verify after EVERY inline push: dst,
sync_stop, the M5b sentinel resume fields, both registry lookups, the
callee's range lookup - a mismatch is a located abort. MYLANG_NOREC_AUDIT
(or g_norec_audit) re-verifies EVERY live record per push - the full-stack
audit. Lever: norec (site emission off). Measured reach: norec_verify ==
sync_inline exactly (1,352,549 on 10_recursion_deep); the audit re-walked
305,676,074 frames on that bench, all consistent.

WHAT THE NET CAUGHT ON ITS FIRST RUN: my own purge loop dereferencing
entries whose owning chunk had died (ASan UAF, jit_norec_register) -
fixed by ADDRESS-RANGE-only purges, hooked into NativeCode::release
(fragments are munmapped on chunk death; a recycled mmap range must evict
stale entries).

SABOTAGES: dst off-by-one, dropped record-site association, unregistered
switched address - each watched failing with a named NOREC SHADOW
MISMATCH. NOT falsifiable at step 1: a corrupted ret-address OFFSET - the
verification's lookup keys come from the same field, so it is self-
consistent by construction. THE FIRST THING STEP 2 MUST DO is compare a
REAL stack-derived return address against the table, which is exactly the
check that closes this hole.

TWO SHAPE-EATERS found writing the -rt test, now in its comment: a FIRST
descent is all new record-stack peaks (the reuse guard declines every
level - a single deep call yields ZERO inline pushes), and direct SELF-
recursion is sync-emitted only with the native stack armed (the ASan lane
runs cap 32, unarmed) - the test drives MUTUAL recursion in a loop.

LANES RUN 2026-08-10, and the push found TWO MORE step-1 bugs first
(the maintainer pushed for backup; CI failed):
- macOS: jit_norec_release_range declared outside ML_JIT_SUPPORTED,
  defined inside - declared-never-defined static, -Werror. The non-JIT
  platform probe caught the same thing locally before CI was read.
- Linux Release: SIGSEGV AT EXIT - the registries and g_func_chunks live
  in different TUs, and the exit-time Chunk destructors erased from maps
  the exit handlers had already destroyed (unspecified static destruction
  order; the CI core-dump net produced the exact backtrace). Fixed by
  making the registries IMMORTAL (construct-on-first-use, deliberately
  leaked). REPRODUCED LOCALLY in rel-hard once the RAW EXIT CODE was
  checked: `./mylang -rt | grep "Tests passed"` reports the GREP's exit,
  and the suite had been printing PASS and then segfaulting in the exit
  handlers. CHECK `-rt`'s RAW EXIT CODE, not a grep through it.
After the fixes: dbg/clang/rel-hard/stats all 1867/1867 with raw exit 0,
corpus_diff plain + audit-on + norec-lever configs agree, non-JIT probe
builds green on g++ AND clang, CMake Debug builds + passes.

## G1 no-record tier STEP 2 seed (2026-08-10): the hardware return address
## falsifies the table

The step-1 record said it plainly: a corrupted ret-address OFFSET was
self-consistent, because every lookup key came from the entry's own
field. This closes that hole with the only key the entry cannot supply -
the HARDWARE one. At the top of EVERY fragment entry (frag_entry, TESTS
builds), [rsp] - the return address the caller's `call` just pushed - is
handed to jit_norec_ret_verify: a C++ address (jit_enter and friends)
resolves to no fragment and passes; an address in EMITTED code means a
fragment-to-fragment call, and the table must resolve it to a site
recording EXACTLY that address, or abort.

Two fragment-to-fragment call forms exist and BOTH are now in the table:
the M5b sync `call rdx` (two addresses per site) and the #55 native-leaf
`call rcx` - the leaf sites are registered address-only, marked `leaf`
(no record is pushed there; that is the leaf protocol's point). Without
them the entry check would false-abort on every leaf call.

SABOTAGE 2 RE-RUN, now WATCHED FAILING: the off-by-one ret-address
offset that step 1 could not catch dies with "NOREC RET MISMATCH: RA
0x... is in emitted code but resolves to no site" on the first -rt run.
The table's addresses are no longer self-certifying.

Reach: norec_ret == norec_verify EXACTLY (1,352,549 on
10_recursion_deep; 149,998 on 45_gcd) - every sync call's return address
resolved through the table on its callee's entry.

COST, stated for the record: the dbg -rt lane 17s -> ~23s (one helper
call per fragment ENTRY, TESTS builds only; a release build emits none
of it). Per the agreed testing plan, a slow test's frequency is the
maintainer's call - flagged, not trimmed.

Lanes: dbg/clang/rel-hard/stats 1867/1867 with RAW exit 0, corpus 14/14
plain + audit-on, non-JIT probe green, nested_fuzz 1000 programs.

## G1 no-record tier STEP 2 (2026-08-10): backtraces from the table,
## verified against the record path - and TWO identity bugs found

The record path captures an M5b frame LOC-LESS (the sentinel has no
locs) and the postexit stamp supplies the site later; the table has both
halves of the frame up front. Step 2 verifies each half at the moment the
record-based renderer uses it, with records still authoritative:

- CAPTURE (vm_capture_rec_frame, TESTS): a norec-site record must be
  sync_stop, captured loc-less, its site must carry a baked loc, its
  desc->vm_chunk must equal run_chunk (the descriptor-from-range
  derivation the record-less renderer will use), and the range lookup
  must resolve its chunk. The popped site is STASHED for the stamp check.
- STAMP (jit_sync_postexit's pending tail, TESTS): the baked
  site_packed the render path stamps into the loc-less frame must EQUAL
  the table's site_loc for the frame the walk just popped - the two
  travelled from one emission through entirely different machinery (an
  emitted immediate argument vs C++ bookkeeping). One stash per walk;
  upper -2 conveyance levels see null and skip.
- CHAIN (the full-stack audit): each emitted-pushed record's site
  caller chunk must equal the frame BELOW's run_chunk - the interleave
  identity the step-3 mixed walk will stand on.

THE CHAIN CHECK PAID FOR ITSELF ON ITS FIRST RUN, twice. Chunk objects
MOVE after compilation, and every site's `caller` (plus the range
registry's chunk pointer) went stale at the move:
- vm_execute's retained VmProgram vector (the return-value move AND the
  vector's own reallocation moving EARLIER programs' roots);
- the lazy vm_func_chunk net, which jits a STACK-LOCAL chunk and
  emplaces it into g_func_chunks.
Both aborted -rt with a visibly-stack `0x7ffc...` caller address. Fixed
by `jit_norec_rebind(Chunk&)` called at every move DESTINATION (the
retained list - ALL entries, the map emplace, vm_install_func_chunk, the
script driver's prog assignment); the chain check remains the net for
any missed move site. This is precisely the class of stale-identity bug
the step-3 walk would otherwise have met as a wild pointer mid-unwind.

Sabotage watched failing: site_loc+1 at emit dies with "stamp != table
site_loc" on the first -rt run. The -rt test gained a throwing phase
(mutual recursion, struct exception, caught at top, driven 4x past the
cold-grow) asserting frame_verify and stamp_verify both advanced.
Counters: norec_frame / norec_stamp, in coverage + MYLANG_JITSTATS.

## G1 no-record tier STEP 3a (2026-08-10): the rbp chain, shadow-walked

The frame-pointer chain the maintainer chose lands here. Every fragment
now maintains rbp - `push rbp; mov rbp, rsp` at frag_entry, `pop rbp`
before every `ret` (the entry-pad parity flipped: 2 pushes + saved, not
1). So [rbp] = the caller's rbp, [rbp+8] = the return address, and a
contiguous run of fragment-to-fragment calls is walkable with two loads
per level - which is the whole mechanism step 4 reconstructs frames from.
rbp was audited FREE: the Reg enum omits 4/rsp and 5/rbp, no emitted
instruction encodes it, and every `ret` funnels through frag_ret (the
audit is `grep u8(0xC3)` -> exactly one, inside frag_ret).

THE SHADOW WALK (jit_norec_push_verify, TESTS): every push now also
receives the pushing fragment's rbp and walks the REAL chain, requiring
it to match the record stack frame-for-frame - each frame's site return
address at [fp+8] resolving to the record's site, and the record's
stored anchor equal to the chain link [fp]. The anchor is a record field
(native_rbp) written by the emitted push UNDER TESTS ONLY - step 4
removes the record for these frames, so a stored anchor has no release
consumer; release pays only the rbp prologue.

THE INVARIANT THIS TAUGHT, and it is the one step 4 stands on: the rbp
chain covers only the TOPMOST CONTIGUOUS NATIVE SEGMENT. A C++ return
address is that segment's floor - a fragment entered from the interpreter
(jit_enter), or a deeper call that declined to the C++ tier because the
sync depth cap runs interpreted-flat (exactly ackermann, which aborted
the first over-eager walk). The record stack continues BELOW with earlier
segments the rbp chain cannot reach. So the walk stops at the first C++
RA and asserts nothing about the record there; the cross-segment ORDER is
step 4's job, checked with the records present.

Reach: 10_recursion_deep walks 304,323,525 frames (the deep segment per
push); a depth-1 call (45_gcd, 76) walks 0 - its caller is C++, so the
walk stops immediately, correctly. Sabotage watched failing: storing the
anchor as rbp+8 dies "callee anchor != the push rbp" on the first run.
The -rt test asserts walk_frames >= the call count on the mutual
recursion, so a walk silently stopping at frame 1 fails.

COST (callgrind Ir, OPT=1 ASSERTS=0, one 1-vs-1): 10_recursion_deep
+1.98% (the 3-instruction prologue per recursion level), 01_while_loop
+0.00%, 09_fib_recursive +0.04%. The regression is confined to pure
fragment-recursion - the exact path step 4 relieves of the ~51-
instruction record fill, which dwarfs this. Steps 1-3 buy nothing by
design; this is the mechanism, not a win.

## G1 no-record tier STEP 3b (2026-08-10): the descriptor chain

A backtrace frame NAMES its function, and 3a left that the one thing the
table could not supply: the site carries the CALLER of a call, never the
CALLEE. So how does step 4 name a reconstructed frame whose record is
gone? Through the frame BELOW it. A call recorded at site S was made from
the function whose chunk contains S - S.caller_desc, KNOWN AT EMIT TIME
(jit_compile_chunk's JitCtx names the function being compiled; null for
main). And the frame a call creates sits directly ABOVE its caller. So:

    desc(frame N)  ==  site(frame N+1).caller_desc

- each frame's descriptor is reconstructible from the site of the frame
one closer to the top. main has a null descriptor and a null-desc
boundary record, so null == null closes the bottom of the chain.

STEP 3b adds caller_desc to NorecSite and, in the shadow walk, checks
that continuity against the still-present records:
records[k].site.caller_desc == records[k-1].desc, at every native frame
and ACROSS the segment floor (the bottom native frame's caller is the C++
frame below, and its desc must match too - the gluing point is a desc
match, not just a pc). This is exactly the reconstruction step 4 performs;
proving it here, record-checked, is the dress rehearsal.

Reach: norec_desc == norec_walk on both 10_recursion_deep (304,323,525)
and 69_exc_crossframe (2,719,864 - the throwing path, where the frames
are actually rendered into a backtrace). Sabotage watched failing:
baking a wrong caller_desc dies "site caller_desc != the frame below's
desc (0x1 vs (nil))" on the first -rt run. The -rt test asserts the
desc-chain counter advanced >= the call count on the mutual recursion.
caller_desc is a compile-time constant of the site, so it is stored in
EVERY build (unlike the shadow-only native_rbp) - step 4's reconstruction
reads it in release.

WHAT 3b DELIBERATELY LEAVES TO STEP 4: the native-SP interleave ordering
(design doc point 2). It only matters once record-less frames coexist
with record-ful ones in one backtrace, which does not happen until step 4
removes the records - today every frame still has a record, so the record
stack IS the order. 3a proved each segment matches its rbp chain, 3b
proves each frame's descriptor reconstructs; step 4 removes the records
and merges the reconstructed frames with the surviving record-ful ones by
SP.

## G1 no-record tier STEP 3c (2026-08-10): the window chain + the
## step-4 gate, measured - and the interleave field 3b deferred is MOOT

The last shadow piece before behaviour changes, plus the protocol read
that settled step 4's design (recorded in full in
plans/archived/g1-no-record-tier.md "STEP 4 DESIGN FACTS").

**THE WINDOW CHAIN.** frag_entry pushes rbx (the frame-base register)
FIRST after `push rbp` - so `[fp-8]` of every native frame is the
WINDOW of the frame BELOW (the caller's rbx, saved by this frame's
prologue). That answers the mixed walk's central question - "does this
frame have a record?" - as `top_rec.window == the frame's own window`,
with the walk learning each next window as it descends: ZERO new record
fields, in any build. The native-SP interleave field 3b deferred to
step 4 is therefore NOT NEEDED. The shadow walk now verifies
`records[idx-2].window == *(fp-8)` at every native-to-native level
(g_jit_norec_win_chain, lockstep with norec_desc; 53,328 on fib's audit
run). Sabotage watched failing: a checker reading [fp-16] aborts with
"frame-below window != [fp-8]". The rbx-first order is LOAD-BEARING and
commented in frag_entry; an emitter-side reorder sabotage is unfireable
TODAY (no sync-call-containing fragment pins cache regs, so `saved` is
empty in every walkable frame - verified by -vdj on a loop+recursion
shape after the optimizer ate the first probe's foldable loop), but the
standing walk catches a reorder the day a pinning fragment appears.

**THE STEP-4 GATE, MEASURED.** The protocol read found a gate the plan's
reach table could not see: THE RECORD IS THE INTERPRETER'S ONLY RESUME
VEHICLE (jit_sync_postexit drives vm_dispatch through it), so a
record-less frame may never exit its fragment mid-body - its chunk must
be FULLY DELETED (every op EnterNative). Classified per M5b inline push
in push_verify (zero extra emit; MYLANG_JITSTATS `norec_gate_*`):
45_gcd / 76_funcval_dispatch / 78_typed_param_call **100% gate_ok**,
09_fib 100% cached (the designed decline), 10_recursion_deep and
69_exc_crossframe **50%** - the recursion partner (`sumto$0`) keeps ONE
interpreted CallV island, so its frames keep records. That corrects the
plan's "up to 99.8%" on those two benches; the fix (nativize the
partner's residual op) is a separate later task. The -rt test asserts
the gate ADMITS the mutual-recursion shape (a gate that silently admits
nothing would otherwise surface only when step 4's tier never fires -
the prove-the-code-ran rule, applied one step early).

**ALSO ESTABLISHED** (design doc 4c-4e): the record-less call/return
protocol (a 2-push residue [dst_addr][captures] AFTER the M5a switch;
the callee restores seg-top/used from its own baked totals, the caller
restores vframe from its; return discrimination by top_rec.window vs
rbx), the raise helpers' new rbp argument, the segment-local mixed walk,
and the -3/SWITCH hazard: the interpreted-flat protocol resumes callers
through their records, so the slow helper's flat arm must MATERIALIZE
records for record-less frames on the rbp chain before going flat (cold,
cap-exceeded only). Step 4 builds in five sub-steps, 4-i..4-v, each
lever-gated.

## G1 no-record tier STEP 4-i (2026-08-10): resume_pc + the real gate -
## and the resume-semantics lesson the first assertion taught

Mechanical prerequisites of record removal, both shadow-verified.

**NorecSite::resume_pc** - the POST-CALL entry-stub pc the flat
(JIT_RET_SWITCH) driver re-enters the caller at, which the -3
materializer (design 4d) needs to rebuild a record-less frame's record.
ONE local at the emit site now feeds both the site field and the slow
helper's rdx (previously computed inline), so the two readings of "where
does this call resume" cannot drift. Leaf sites bake none (a leaf callee
makes no calls, so no -3 originates below one).

**jit_chunk_norec_ok** (jit.cpp, beside jit_chunk_is_native_leaf; stub
false off-platform) - the real step-4 callee gate: plain_frame + fully
deleted + ref_slots <= RET_REF_GUARD_MAX, per-call exclusions left to
the site. The push_verify reach classifier now calls IT instead of its
inline mirror (which duplicated the ref bound as a literal - the exact
audit-table drift hazard); reach numbers byte-identical after the swap.

**THE LESSON, watched first-hand:** the registration-time verification
of resume_pc initially asserted "names an EnterNative entry stub"
unconditionally - and aborted within one -rt run on a KEPT-run site
(resume_pc 1 of 3 ops). The real semantics: in a NON-deleted caller the
interpreted original at that pc IS the resume target (vm_dispatch just
interprets from there); the stub is guaranteed only where originals are
deleted - which is precisely the gate-passing callers whose sites the
materializer will consume, so the conditional form verifies exactly what
step 4 depends on. A belief about the resume protocol was falsified by
its own verification net before any code depended on it - the arc
working as designed. Sabotage (bake resume+1): "out of bounds" abort on
10_recursion_deep's first registration.

Lanes: dbg/clang/rel-hard -rt 1867/1867, corpus 14/14, non-JIT probe
(g++ + clang) builds and passes, plain release builds and runs.

## G1 no-record tier STEP 4-ii(a) (2026-08-10): the raise-time anchor -
## the raise helpers carry the fragment's rbp, walked at every native throw

The record-less unwind will anchor on the raising fragment's rbp; this
step proves that anchor LIVE, with records still present. jit_throw /
jit_rethrow / jit_end_finally gained a trailing frag_rbp argument
(`mov rcx/r8, rbp` at the emit - raw bytes, the Reg enum deliberately
omits rbp), threaded to vm_raise (a defaulted parameter, so the ~15
interpreted raisers are untouched and pass null). vm_raise runs the
SHARED chain walk - factored out of the push verification into
norec_walk_chain - from the raise point: the same traversal at a
DIFFERENT moment, after arbitrary body execution, which is what proves
rbp still holds the fragment's frame (callee-saved + never emitted)
exactly where step 4's walk will read it.

TWO counters, and the split is the point: norec_raise_walk counts
invocations, norec_raise_frames the LEVELS traversed. A dead anchor
(rsp, a clobbered rbp, the wrong argument register) floors the walk at
level 0 SILENTLY - the invocation count alone stays green. The -rt
assertion requires both; the sabotage (mov rcx,rsp in jit_throw's emit)
was watched failing as "4 raise walks traversed 0 frames - the raise
anchor is dead", 1866/1867.

Reach, per probe: a deep mutual-recursion throw walks ~14 frames per
raise (norec_walk 680 -> 765 over the pre-4-ii binary on the same
program). 42_exceptions' 200k and 69_exc_crossframe's 20k raise walks
all floor at level 0 - CORRECTLY: 42 throws in main's fragment
(jit_enter, C++ RA), and 69 is native-top / interpreted-middle /
native-bottom (deep's self-call declines the sync tier), so its
throwing frame is a 1-frame segment entered from C++. The walk's floor
rule is doing exactly its job on both.

Remaining in 4-ii: (b) the unwind CAPTURE path consumes the chain -
reconstruct each popped frame's backtrace entry from the site and
cross-check byte-for-byte against the record-based capture.

Lanes: dbg/clang/rel-hard -rt 1867/1867, corpus 14/14, non-JIT probe
(g++ + clang), plain release runs.

## G1 no-record tier STEP 4-ii(b) (2026-08-11): the end-to-end
## reconstruction compare - and the loc-less frame [0] it discovered

The last verification piece before behaviour changes: a complete
backtrace prefix RECONSTRUCTED from hardware + baked emit-time constants
only - zero record fields - compared frame-for-frame against the
exception's real captured backtrace.

The missing datum was the RAISING function's identity: a site names its
CALLER, so no site can name the frame that contains the throw. The raise
helpers (jit_throw / jit_rethrow / jit_end_finally) now bake it -
g_cur_caller_desc at the throw emit, one movabs (null for main, matching
main's null-desc boundary record). vm_raise builds `g_norec_recon`: the
baked desc seeds the descent, each level contributes {desc,
site([fp+8]).site_loc}, and the next desc is that site's caller_desc -
the 3b recipe finally producing capture-shaped data. Each level is
cross-checked against the record that still exists (which also proves
the desc bake, suite-wide); the -rt harness then throws uncaught through
a WARM 21-frame native segment (a try-loop warm-up defeats the cold-peak
shape-eater: iteration 1's pushes all decline on the record-reuse guard)
and compares the recon against the caught exception's backtrace.

**WHAT THE FIRST RUN OF THE COMPARE TAUGHT** - the reason composition
checks exist: every index matched EXCEPT frames[0].call_site == 0. The
innermost frame's M5b capture is loc-less and its stamp never lands -
jit_sync_postexit reads `d = back_rec().desc` AFTER the raising frame's
walk already popped it, so the stamp's `back().desc == d` guard compares
the callee's frame against the CALLER's desc and skips. UNOBSERVABLE by
design: format_backtrace renders frame [0]'s line from ex.loc_start and
never reads its call_site - so this is not a bug to fix but a fact to
pin. The compare pins the stored 0; step 4's capture rule is therefore
"leave [0] loc-less" for byte-identical backtraces. The recon knows MORE
than the record path materializes.

Sabotage watched failing: a post-build std::reverse of the recon
satisfies every per-level check (desc chain, window chain, record
cross-checks - on an alternating recursion it even keeps the desc
column aligned) and fails ONLY the end-to-end compare ("recon frame 20
diverges", the loc sequence betraying the order). That is precisely the
class of bug - assembly, not fields - this net was built for.

Lanes: dbg/clang/rel-hard -rt 1867/1867, corpus 14/14, non-JIT probe
(g++ + clang), plain release runs. 4-ii complete; next is 4-iii - the
record-less return arm + call-site residue behind MYLANG_JIT_FORCE=norec,
where behaviour first changes.

## G1 no-record tier STEP 4-iii (2026-08-11): the record-less return arm
## + the call-site residue, behind MYLANG_JIT_FORCE=norec

The first behaviour-changing step, and the shadow apparatus earned its
keep FIVE times before the lanes went green.

**The mechanism.** Under FORCE, every emitted sync site pushes the
2-qword residue ([dst_addr][captures]) around `call rdx` - after the
M5a stack switch (it must live on the stack the callee's rbp sees),
parity kept. The caller's OLD ctx.captures is relayed through a global
(g_jit_residue_caps): it is only readable inside the push's fill, and
no register survives the intervening ref-bind helper calls. The push
marks the record with the CALLEE's `Chunk::norec_ok` byte (a DERIVED
flag recomputed by a scope guard on every jit_compile_chunk exit) - the
runtime half of the gate, since the callee is dynamic at emit time. The
callee's return discriminates on that byte (after the boundary check)
and its record-less arm sources everything the record supplies from
elsewhere: the result through the residue's dst_addr (none for Halt),
vframe.slots from [rbp-8] (the window chain), seg-top/used as BAKED
chunk-total subtractions (equal to the record path's absolute restores
by the seg-top induction), the release scan from its own ref_slots, and
the resume-global stores SKIPPED (a residue frame's caller is native by
construction). Two bookkeeping stores (rec_n--, top_rec) keep the still-
written records consistent and die in 4-v. The caller's sentinel arm
restores ctx.captures + vframe.size - baked for a function caller, via
its own PERSISTENT boundary record for main. jit_norec_retarm_verify
cross-checks residue-vs-record field-for-field on every arm entry.

**WHAT THE NETS CAUGHT, in order:**
1. **The mid-body re-entry hole** (the oracle's first forced -rt): a
   residue frame whose fragment exits mid-body is re-entered via
   jit_enter - the record survives, the residue died with the original
   native frame, and [rbp+24] is garbage. Fix: the runtime norec_ok
   gate (a not-fully-deleted callee can always exit mid-body) plus a
   one-byte flag clear at the EnterNative C++ entry (covers the
   -3/flat abandonment too).
2-4. **THREE dead-tier finds**, each by the prove-it-ran counter
   reading 0 while every result stayed correct - the exact failure
   mode the counter rule exists for:
   - the op-level CachedCallV exclusion zeroed gcd (its whole recursion
     is a CachedCallV whose cache never engages; the tier's exclusion
     is a RUNTIME property, now the arm's cache_key/caller_cache
     guards);
   - the main-caller exclusion zeroed EVERY headline bench: their outer
     call loops all live at top level, and gcd's recursion is
     tail-spliced into a loop so main makes ALL its calls;
   - the res_slot >= 0 gate zeroed 76_funcval_dispatch: fall-through
     bodies end in HALT, not ReturnV (visible in one -nj -vd), so the
     arm now writes the none singleton for them.
5. **An off-platform break** (jit_norec_forced without a stub), caught
   by the non-JIT probe - the 21abce2 class again.

**Reach under FORCE** (scale 1): gcd 149,998/150,000 returns through
the arm, 76 999,999/1M, 78 100%, 10_recursion its whole sync portion;
fib declines via the cache guards as designed. Sabotage watched
failing: swapped residue push order -> "residue dst_addr !=
&parent[rec.dst]" abort.

**Lanes, BOTH modes** (the default binary is byte-identical when
unforced - every 4-iii emit is behind jit_lever_forced):
dbg/clang/rel-hard -rt 1867/1867 forced AND default; forced corpus
14/14; 300-program nested_fuzz forced (5 engines agree); plain release
runs forced (the arm works without the oracle); non-JIT probe. The C4c
coverage test now counts g_jit_ret_inline + g_jit_norec_ret_arm - the
property is "served inline, not by the jit_ret helper", which either
arm satisfies.

## G1 no-record tier STEP 4-iv (2026-08-11): the -3/SWITCH materializer
## - and the LOST-CONTINUATION BUG it unearthed (#148)

The step's plan (design doc 4d) was to build the mechanism that keeps
record-less frames resumable across a depth-cap SWITCH. Preparing its
test exposed that the -3 protocol was ALREADY broken for record-FUL
frames, from the day it shipped: after a switch, the -3 propagation
unwinds every native/C++ frame of the sync chain, but each frame's
record still carries the SENTINEL resume (sync_stop, ret = the stop
chunk) - meaningful only while its C/native consumer waits. When the
flat continuation's first pop past the switching caller read one, it
dispatched the stop chunk's ExitBlock, vm_dispatch RETURNED, and the
whole rest of the program was silently ABANDONED with exit 0:
`print(111); var r = aa(runtime(50)); print(222);` printed only 111 on
any build whose cap is below the depth (the ASan lanes run cap 32).
Verified BORN BROKEN at the protocol's introducing commit (f080b99).

**Why every net missed it**: the coverage test's asserts sat AFTER the
deep call, and a skipped assert reads as a pass - the test was VACUOUS
from birth (only its g_jit_sync_switch growth check ever bit). The
THROW half survived by luck: a raise walks the records downward in one
C++ sweep to the handler, needing no per-frame native resume. The test
now CAPTURES STDOUT and compares - a print cannot be skipped invisibly.

**The fix IS the materializer, applied one class earlier than planned**:
at the switch, every doomed sentinel record is RETARGETED to its real
resume - (site.caller, site.resume_pc), the caller's post-call entry
stub, the exact fields a SWITCH-pushed record carries - after which the
existing machinery (vm_frame_leave / the C4c inline pop reading
rec.ret_* into the resume globals, the EnterNative SENTINEL consumer
dispatching them) resumes each caller natively, link by link, back to
main. Division of labor:

- `norec_switch_retarget` (vm.cpp, RELEASE code): the 3c walk's release
  twin - records paired positionally with native frames from the
  relayed rbp, the site via each frame's return address
  (jit_norec_site_for). Covers the M5b (call rdx) frames of the topmost
  segment. Stops at a non-sentinel record (a live consumer) or a C++
  RA (the segment floor). Also clears sync_stop (a later raise walks
  through normally), stamps call_site_packed = site.site_loc (the
  deleted-run caret), and clears the 4-iii residue flag (the residue
  dies with the native frames).
- `jit_call_sync_core`'s r == JIT_RET_SWITCH branch retargets the
  record IT pushed from its own (caller_ck, resume_pc) args - its C
  frame IS the dying consumer, and no site can name a core-pushed
  frame's resume - then walks the CALLER's segment from the relay
  anchor it snapshotted at entry. Induction: each C++ level fixes the
  segment below it.
- **The anchor relay** (g_norec_switch_site/rbp, consumed-and-nulled by
  the wrappers): the emitted slow tail stores its NorecSite* + the
  fragment's rbp just before the helper call (all six argument
  registers are taken). This is the permanent 4-v mechanism, emitted
  unconditionally when the site exists.
- **The generic dyn-callee path** (CallValueGenericV) bakes no resume
  stub, and its site mistranslated a propagated status 3 into a re-run
  bail (observed: a double-call abort). It now CONSUMES a switch
  instead of propagating: jit_call_value_generic drives the flat
  continuation in its own dispatch invocation, which thereby becomes
  the sentinel consumer of the record core pushed - core's own
  protocol one level up, zero emit changes. Frames above it are never
  doomed.

`norec_materialize_shadow` (TESTS) runs at each switch BEFORE the
retarget: the full insert reconstruction per frame (identity descent
seeded by the relayed site, window via [fp-8], dst/nslots/seg
math/captures-from-residue vs the pristine records; the residue half
under MYLANG_JIT_FORCE=norec). Counters norec_mat_walk / mat_frames /
mat_residue; on the 3-descent probe: 3 walks, 67 frames, 64 residue
reconstructions forced - and sync_inline went 0 -> 85, because the fix
un-broke the M5b inline pushes of every re-descent (the record
HIGH-WATER gate declines a first descent; a re-descent over warm
records inlines, and its chains previously died at the first switch).

**Retargeted-record consumers taught**: the full-stack audit and
vm_capture_rec_frame skip/re-pin on sync_stop == 0 (a retargeted frame
captures from its packed site loc, byte-identical to a SWITCH record).

**Sabotage, all three watched failing**: the walk disabled -> only the
core-chain descent survives ("stdout 60" vs 60+31); core's retarget
disabled -> stdout EMPTY; the relay storing rsp instead of rbp (a dead
anchor) -> the re-descent lost. Three nets, each naming its failure.

**Residuals filed separately** (pre-existing, reproduced at HEAD's
build, NOT switch-related): #149 - `var dyn g = aa` (a chunk-less
TEMPLATE BASE) aborts at the call after AST teardown; #150 - the AST
inliner asserts on a lambda calling a MUTUALLY-recursive template.
Under MYLANG_JIT_OFF=norec no sites exist, so M5b-framed chains retain
the historical hole there (core chains are fixed regardless) - a debug
lever, documented status quo.

**Lanes, BOTH modes**: dbg/clang/rel-hard -rt 1867/1867 default AND
forced; corpus 14/14 both; 150-program nested_fuzz both (5 engines);
non-JIT probe (g++ + clang); a .myv image of the deep-switch shape
resumes identically (the load-time JIT rebuilds the sites). Interleaved
full-suite bench cur/base 0.999x (the retarget is cold-path only).

## G1 no-record tier STEP 4-v incs 1-4 (2026-08-11): records actually
## stop being written - behind MYLANG_JIT_FORCE=norec

Seven commits (cb8b838..eb8c36d; design facts in the plan's "STEP 4-v
DESIGN FACTS" + "4-v-ii REFINED" sections - read those first):

- **inc 1**: every record carries its PARENT's view (window/nslots/seg,
  captured at push), replacing every "records[rec_n-2] is my parent"
  read - pop_window's repoint/cur_seg and the C4c arm's parent loads.
  Byte-identical in the all-record world, oracle-pinned per push,
  sabotage watched failing (on the WARMED mutual-recursion shape - the
  self-call probe was vacuous, a self-recursive CallV is run-excluded
  from the sync emit).
- **inc 2**: the record-less RAISE arms, conveyance-uniform (ONE frame
  per level - a prefix sweep double-cleans, since the native unwinding
  still crosses each swept frame's site): vm_raise cleans a record-less
  RAISING frame from live+baked state; jit_norec_postexit cleans a
  record-less EXITED callee (identity from the EXIT RELAY - the
  fragment epilogue's last store; window/total from the vframe; caller
  captures from the residue relay, now vm.cpp-owned). The -2 arm's
  stamp passes a null desc so frame [0] stays loc-less exactly as the
  record path's after-the-pop desc read makes it.
- **inc 3**: the switch materializer INSERTS full records for
  record-less frames (window chain + sites + residue + live watermarks
  + the parent view), seeded by the relay site; core seeds from its
  entry-time captures. After materialization the -3 flat continuation
  is the plain record world.
- **inc 4, THE FORK**: a gate-passing call (norec_ok = plain + fully
  deleted + short ref_slots + NO CachedCallV in the pre-deletion body,
  scanned by NorecOkGuard's ctor; no parked key; no live caller cache)
  skips the record fill entirely. The return arm discriminates by the
  WINDOW COMPARE - which MUST PRECEDE the boundary byte test (the top
  record is an ancestor's; main's boundary bit hijacked every
  record-less return - norec_ret_arm 0 vs 39 pushes, then a Frame::at
  abort at main's print with the callee's stale vframe.size). Declines
  go to jit_ret_norec; the caller's sentinel arm restores
  vframe.slots=rbx + size. TESTS nets forked per mode (the
  record-pairing shadows gate off under FORCE; their replacements:
  norec_pushes / the arm counter+oracle / norec_mat_insert).

**Measured** (OPT=1 ASSERTS=0, forced-vs-unforced interleaved):
callgrind whole-program 10_recursion -12.2%, 78 -6.1%, gcd -3.6%,
76 -3.0% (~27-30 Ir/call net of the gate+residue); wall 10_rec 0.75x,
78 0.88x; suite geomean 1.011x (the non-call benches pay ~1% for the
per-push gate+residue). The DEFAULT FLIP (inc 5) is the maintainer's
call with these numbers - the tax is shrinkable first, Net 4 remains,
and 10_recursion's full reach wants sumto$0's CallV nativized.

Lanes, both modes: dbg/clang/rel-hard -rt 1869/1869, corpus 15/15,
150-program forced fuzz. Reach probes: 39/39 record-less on the
alternating value-call shape, 85/85 on the 3-descent switch shape with
the materializer inserting what the -3 path resumes through.

## G1 4-v TAX SHRINK (2026-08-12): the broad-suite cost halves

Four removals (full detail in the plan's "4-v TAX SHRINK" section):
the 4-iii rec_residue byte end to end (including the per-EnterNative-
dispatch clear that ran in EVERY mode - the window compare is
self-truthing); the record path's caps-relay park (a record-ful
callee's residue captures value is provably unread); the caller
sentinel arm's captures round-trip (the record-less return arm and
jit_ret_norec restore ctx.captures from the residue themselves); the
plain-site pending-key gate test (only a CachedCallV site's own probe
can park a key). Re-measured: callgrind 10_recursion -14.5%, 78 -7.4%,
gcd -4.3%, 76 -3.7% (~33-36 Ir/call net); suite geomean forced/
unforced 1.006x, inside the run-to-run spread.

**The shrink exposed a coverage gap** (the vacuous-test trap): a
sabotaged captures-restore offset PASSED the entire forced suite - no
test read a CLOSURE CAPTURE after a record-less return. The new
closure-capture test constructs the shape, defeating two shape-eaters
(a tiny callee INLINES -> the mutual pair; a write-once capture
AUTO-CONSTS and the read folds -> the write-twice init; also noted:
`var c = runtime(10)` is DynRequiredEx and `int c = runtime(10)` a
compile TypeMismatchEx - the dyn coercion needs the two-statement
spelling). Watched failing; both return tiers covered (the emitted arm
via a scalar result, jit_ret_norec via an array result).

## G1 4-v INC 5 (2026-08-12): the no-record tier is the DEFAULT

jit_norec_forced() -> jit_norec_on() = !jit_lever_off(JL_NOREC).
MYLANG_JIT_OFF=norec is the same-binary A/B lever; FORCE=norec a no-op.
The TESTS nets self-adjust per mode, so `-rt` is green with the tier
ON (the fork's own proofs: norec_pushes, ret_arm, mat_insert) and OFF
(the full record-pairing shadow battery) - keep running BOTH in the
lanes, the OFF mode is the record path's only -rt coverage.

THE FLIP'S ONE DEFECT: the NorecSite side table was lever-gated while
the emitted entry-RA check (jit_norec_ret_verify, every TESTS fragment
prologue) resolves against it in every mode - OFF=norec aborted -rt at
the first fragment-to-fragment call. Fixed by making the table
UNCONDITIONAL (passive address data; the lever disables behaviour -
the push fork, the return arm, the residue - never data). OFF=norec is
now byte-equivalent to the pre-flip default. Perf numbers carry over
from the TAX SHRINK entry (the forced config already built the table).
Lanes: dbg/clang/rel-hard -rt 1870/1870 both modes, corpus 15/15 plain
+ levers, 40-program fuzz clean.

## H1 (2026-08-12): a typed CAPTURE leaf keeps its arithmetic UNBOXED

A closure capture lives in `ctx->captures`, not the frame, so it can
never BE an `Operand` (a literal or a frame slot) - `as_int_operand` /
`as_float_operand` decline it. That decline PROPAGATED: one capture
operand pushed the whole TypedScalarExpr onto the boxed tier, so
`func [base] (int k) { return base + k; }` - proven int end to end by
inference - compiled to `load.capture` + a BOXED `bin.v`, i.e.
num_bin_op's PMF dispatch + promotion check per call. The tree-walker
had no such hole (`Identifier::eval_int` reads a capture directly);
only the codegen did.

The fix needs NO new opcode: `try_capture_leaf` (codegen.cpp)
MATERIALIZES the capture with the existing boxed LoadCaptureV into a
temp and hands that temp back as the operand. A temp holding a boxed
int IS an int frame slot - `read_int_slot` reads it by tag like any
other, and its bool -> 0/1 arm matches `Identifier::eval_int`'s capture
arm byte-for-byte. Only the ARITHMETIC changes tier; the capture load
(and its JIT inline copy) is untouched. The float twin accepts an `i`
capture too, exactly as `as_float_operand` does for a local
(read_float_slot promotes).

Measured (OPT=1 ASSERTS=0, interleaved --baseline, the full suite):
**78_typed_param_call 0.75x wall / -23.7% Ir per iteration** (889 ->
678, my/cpp 15.6x -> ~11x); suite geomean cur/base **0.999x**. Blast
radius is 2 of 95 corpus programs (a `-vd` md5 sweep): 78, and
67_make_dict - whose `make_dict(ks, func[r](k) => k*k+r)` callback
loses its last boxed op and becomes a #55 **native_leaf** (flat: the
dict is its cost, not the arithmetic). 11_closure_counter and
63_closures are byte-identical - their cost is the capture STORE
(`count++`), which is H1's next increment.

**THE STAGE TRAP, in TEST form (worth reading before writing the next
shape test).** The first version of the coverage test counted
`OpCode::IntBin` / `FloatBin` and read **0/0/0 against correct code**:
by the time `vm_compile` returns, `specialize_arith_ops` has rewritten
those into the B1/B2 register-immediate family (IntAddRR ...). A test
that reads a chunk AFTER codegen must count the FAMILY, not the
generic op - the same "a table is audited only for the stages that
existed when it was written" lesson, one layer up. Sabotage-watched:
reverting `try_capture_leaf` to decline fails the shape test
(int=0 boxed=1 where 1/0 was wanted).

## H1b (2026-08-12): a COMPOUND capture update leaves the helper call

`cap++` / `cap OP= v` on a proven int/float capture lowered to a
COMPOUND StoreCaptureV - `jit_store_capture_compound` -> num_bin_op,
the ONE capture form the JIT still served with a helper CALL (the
PLAIN store has had an inline tier since de-helperize 6b, the compound
never did). It is now RECOMPOSED into the typed tier: H1a's capture
materialize + IntBin/FloatBin + a PLAIN StoreCaptureV. Three inline
sequences instead of one call; no new opcode.

**`+ - *` ONLY, and that IS the soundness argument.** For proven
int/float operands those three cannot throw (`-fwrapv` makes int
overflow defined, float arithmetic raises nothing in-language), so the
recomposition has NO error path and therefore no caret to preserve.
`/` and `%` are excluded on exactly that ground, and the reason is
MEASURED, not asserted: with `/` recomposed, `a /= k` with k == 0
carets **line 4, `var c = mk(12);`** (and reports the wrong backtrace
line) where the tree-walker and the shipped build both caret **line 2,
`a /= k`** - the recomposed IntBin carries no loc of its own, so the
pc lookup degenerates to a neighbour (the #88 shape). A moved caret is
a RULE 2 violation, so the exclusion is now pinned by an `err loc:`
test on that exact program, beside the shape test's decline case.

CAPTURES only, not globals: a capture is always bound (snapshot at
closure creation) so the read cannot raise, while a global read can
(the TDZ's UnboundSymbolEx) and the compound op's undefined-global
bail would have to be reproduced.

Measured (OPT=1 ASSERTS=0, interleaved --baseline, full suite, H1a+H1b
together): **11_closure_counter 0.55x** (my/cpp 18.6x -> 9.08x - out
of the top five entirely), **78 0.80x** (15.6x -> 11.5x), **63_closures
0.90x** (14.2x -> 11.4x), 67_make_dict 0.96x; suite geomean cur/base
**0.986x**, my/cpp geomean **2.424x**. Blast radius 4 of 95 corpus
programs. Sabotages watched failing: storing the pre-update value
(caught by the persistence test), and re-including `/` (caught by both
the shape decline and the caret test).

## H2 (2026-08-12): a proven-array element READ leaves SubscriptV

`x = a[i]` on an array the inferencer proved (`base_array`) with an int
index now lowers to **LoadElemValue** instead of the generic SubscriptV.
SubscriptV goes through the runtime `Type::subscript(for_write=false)`
virtual, which for a general array with an LValue base builds the
element's LVALUE - a `get_vec()` plus two container back-pointer stores
- and `jit_subscript` then `RValue()`s all of it away, because the op
wants a VALUE. LoadElemValue reads the element straight out of the
storage.

`LoadElemValue` became UNIVERSAL to make this safe: it served
general-or-strs and raised InternalErrorEx on anything else (its only
caller was compile_array_base's nested-read base, whose element is
itself a container). A proven array can be FLAT, so both twins now go
through **`vm_arr_elem`, which IS `arr_elem_at`** - the same function
TypeArr::subscript's read path calls - and every kind boxes exactly as
before, from ONE implementation. The op's InternalErrorEx now means
only "the base is not an array", which `base_array` proves.

**TWO AUDIT-TABLE GAPS FELL OUT, and they are the durable part.**
LoadElemValue was missing from BOTH `visit_use_def` (so it was a
LIVENESS BARRIER - every temp read live) and `retargetable_dst` (so the
E1 peephole could not fuse `<produce t>; MoveV d = t`). Neither cost
anything VISIBLE while its only caller was a nested-read base consumed
by the very next op; the moment H2 gave it a plain `x = a[i]` in a
LOOP, both bit at once - the MoveV survived (an extra 32-byte copy +
refcount per iteration) and the barrier killed the call-cluster
dead-dst rule, so 76's discarded `fn(st, i);` went back to
materializing its result. Watched in the disassembly, fixed, and 76's
bytecode is now byte-identical to before except the one opcode. **This
is the third occurrence of the stage trap in this file; the shape is
always "a table is audited for the callers that existed".**

MEASURED, and the projection was WRONG in an instructive way. Ir
(OPT=1 ASSERTS=0, scale-1-vs-3 so compile time is excluded):
**76_funcval_dispatch 836 -> 734 Ir per iteration, -12.2%**;
46_matrix_mult -0.6%. Wall clock (interleaved --baseline, full suite):
**76 is FLAT (1.01x)**, 46 0.97x, suite geomean cur/base 1.004x - and
benches whose bytecode is BYTE-IDENTICAL swing 0.89x-1.13x in the same
run, which is the noise floor this sits inside. The plan projected
76 -20-25% WALL from ~200 Ir; the Ir arrived and the time did not,
because what was removed is a helper-call frame, a virtual dispatch
and pointer arithmetic - cheap, perfectly-predicted, L1-resident work
that retires alongside the memory-bound call protocol. The same
instruction-vs-time divergence recorded for the guard-elision family.
The change stays (fewer instructions, smaller emitted code, and the
two table fixes are correctness-adjacent wins), but **76's wall-clock
gap is NOT in its element read** - the remaining 734 Ir/iteration is
the call protocol and the two arg copies.

## H3 (2026-08-12): the unpack bind drops the type-erased dispatch

75_indexed_unpack's four-way probe (the H2 lesson applied BEFORE
building: prove the time moves) put ~93% of the bench's wall time in
the element BINDS - ~6.7ns/element for what is logically a 24-byte
handle copy plus two refcount RMWs. The cost is the value model's
indirection: `vm_arr_elem` materializes a boxed temp (an intrusive_ptr
retain + a move ctor) and `LValue::put()` runs `EvalValue::operator=`,
an INDIRECT call through `type->move_assign`, plus destroy/create hops
on a type change. The types are statically knowable at the bind site
(a string row yields a SharedStr), but the value model reaches them
through function pointers.

**`vm_slot_bind_str` / `vm_slot_bind_value` (vm.cpp):** when BOTH
sides are strings and the slot is PLAIN (no container back-pointer -
the COW path keeps put()), assign the SharedStr handle directly - an
intrusive_ptr release+retain (self-assign guarded by the pointer
compare) plus three POD fields, fully inline, ZERO indirect calls. The
steady state (iteration 2+) always hits it: the slot still holds the
previous iteration's string. A general-storage NON-string element
improves too: `put(const &)` on the element in place - one copy_assign
- instead of the vm_arr_elem temp (copy_ctor + move_assign + dtor).
Flat scalars keep write_*_slot; structs must materialize. SOUNDNESS:
observably identical to put() - the slot shares the same StrObj either
way (the window model's value semantics are a property of the handle),
put() never tested is_const either, and passing a reference INTO the
array's storage is safe because the base frame slot owns the outer
array for the whole op and the op writes only frame slots. NOTE
bench 75's rows are GENERAL storage (plain string literals stay
general; only split()/splitlines() build flat strs) - the general arm
is the hot one, the flat-strs arm its sibling.

Wired into `vm_unpack_elem_body` (both the consecutive and the
targets loops; `jit_unpack_elem` funnels through the same body, so the
JIT inherits it) and `vm_multi_unpack_body`'s plain stores (array
destructure + scalar spread); the compound and numeric-coerce arms
stay on store() untouched.

MEASURED (OPT=1 ASSERTS=0, interleaved --baseline, full suite):
**75_indexed_unpack 0.84x wall, -25.4% Ir** (1991M -> 1486M at scale
1 = ~50 Ir per bind over 10M binds); suite geomean cur/base 1.002x,
untouched benches swinging 0.91-1.13x in the same run (73's 1.11x and
20's 1.05x sit inside that band and neither touches the changed arms -
20 is the flat-int unpack, 73 int elements through the unchanged
default arm). Unlike H2, the Ir arrived AND the time moved - the
removed work here includes an allocation-class temp plus two refcount
RMWs per element, not just predicted branches.

PROOF + NETS: `g_unpack_fast_binds` bumps ONLY in the dispatch-free
arm; `unpack_fast_bind_shapes` (-rt) asserts growth per shape (general
rows, flat-strs rows, multi-unpack destructure, scalar spread) and
EXACTLY 0 on alternating str/int re-binds. Both sabotages watched
failing: disabling the fast arms fails the counter check; a one-short
window from the fast arm fails the H3 value tests AND the pre-existing
unpack tests + the op-nativized JIT check. Three dual-engine tests pin
the observables (append-to-loop-var never leaks into the row across
the steady-state re-bind, type-changing re-binds stay exact, the
multi-unpack siblings keep their values).

REMAINING SIBLINGS (enumerated, not built): the single-var foreach
VALUE bind (`foreach s in rows` - do_iter / the foreach Next op pays
the same put() chain; the probe's one-bind loop cost 0.16s of the
0.28s shape, the same class of target), and the tree-walker's
bind_loop_var (perf parity only - values already identical).

## H4 (2026-08-12): an INDIRECT call's result is a typed operand

A call through a func VALUE degraded the whole surrounding expression to
the boxed tier. The A/B is one operand wide:

    var q = runtime(3); s = s + q;    ->  i.bin   (unboxed)
    var f = mk(7);      s = s + f(i); ->  bin.v   (boxed)

Nothing was unproven - `-dti` gives `f : func(int)->int`, `s : int`, and
M8 built `TypedScalarExpr<arith,i>(CallExpr)`. Only the codegen dropped
it. The DIRECT call has been a typed leaf all along (`try_native_call`,
whose comment already said "so `s += f(i)` stays the int fast path");
its INDIRECT sibling never was. On 78 that cost a 190 Ir/iteration
helper chain for one float addition.

`try_call_leaf` materializes the result with the EXISTING call op into a
temp; the temp IS an int/float frame slot the typed ops read by tag.
**No new opcode and NO RUNTIME GUARD** - the latter is legal only
because option B (statictype.cpp) made function subtyping compare the
whole SIGNATURE, so a concrete static return type is a real runtime
guarantee. Loosening that rule breaks this tier first, silently.

MEASURED (interleaved --baseline, OPT=1 ASSERTS=0): **78 0.74x wall,
my/cpp 10.87x -> 8.02x, out of the top five**; suite geomean cur/base
1.001x with untouched benches spanning 0.88-1.11x.

**PLACEMENT - three wrong answers, each caught by a test:**
 1. EARLY -> infinite recursion: `compile_boxed_expr` DELEGATES back to
    the typed compilers for a th==i/f node. Hence its `allow_typed`
    opt-out, which exists for this one caller.
 2. LAST-resort -> dead: compile_int_expr returns false at
    `if (!t) return false;` for a non-TypedScalarExpr, before the tail.
 3. Unnarrowed at the right spot -> swallowed `sqrt(i)` from the typed
    MathFnV into the generic CallBuiltinV marshal.
Final: after every specialized typed path, before the TypedScalarExpr
cast. The Direct*-form exclusion is DEFENSIVE (removing it stays green
at this position - watched); the PLACEMENT is what earns the credit.

**THE TEST TRAP, which generalizes past this change:** counting
`IntBin`/`FloatBin` made the shape test read 0 and pass vacuously - the
plain op disappears TWICE before vm_compile returns, fused into
`IntAddStep` (#9) and rewritten by `specialize_arith_ops` into the B1/B2
family (the float case emits **FloatAddRR**). THE AUDIT-TABLE STAGE TRAP
in test form, and the second time it has bitten a test in this arc.
**A test that names an opcode must name the one that SURVIVES to the
stage it inspects.**

## #162 - THE IN-PLACE ARGUMENT: a reference argument is bound straight
## from the caller's slot, and the staging move is not emitted

A call's arguments live in a CONTIGUOUS register run, so an argument
that is already a named local costs a staging `MoveV run[i] = x` whose
only consumer is the bind two instructions later. For a REFERENCE that
move is a `jit_move` call plus the value model's type-erased
release/retain triple. 76_funcval_dispatch:

    23  i.bin        r6 = i % 2
    24  load.elem.v  fn = ops[r6]
    25  move         r6 = st        <-- NO LONGER EMITTED
    26  move         r7 = i         <-- kept: trivial, already 2 stores
    27  call.val     _ = fn(r6, r7) <-- arg 0 read from `st` directly

MEASURED (76, OPT=1 ASSERTS=0, the scale-1-vs-3 delta so compile time
and JIT warmup are excluded from both sides): **734 -> 588 Ir per
iteration, -19.9%**; whole-program -19.4% / -19.7% at scale 1 / 3.
Wall-clock and the suite geomean are recorded in plans/top5-cpp-gap.md.

**IT IS NOT A BORROW, AND THAT IS THE DESIGN.** The investigation that
scoped this (plans/top5-cpp-gap.md, "DIRECTION 1") proposed making the
staging slot NON-OWNING - copy the bytes without a retain, then
neutralise the slot - which drags in `ref_slots`, the release scan,
`jit_ret_audit` and every throwing exit, and buys only the one
retain/release pair (~15 Ir). Not writing the slot at all buys the whole
move, and the run slot then holds exactly what it held before, still
owned by whoever wrote it. **Nothing about ownership changes.**

**SOUND** because the caller slot and the run slot hold the SAME value,
so reading either is correct as long as nothing writes the caller slot
in between - and between the staging moves and the call, nothing writes
anything but run slots. That is also why the fusion survives a branch or
a fragment ENTRY landing anywhere in the sequence: the call reads the
caller slot either way, so a partially-executed staging run cannot
matter.

**THE ONE THING THAT STILL READS THE RUN** is every arm that hands the
call to C++: a guard decline, the depth-cap SWITCH, and the BAIL whose
status 1 resumes the INTERPRETED call op (jit_call_sync_core's
documented idempotent bail). They converge on ONE join in
emit_sync_call_inline, and `jit_stage_args` materialises the run there
from the site's baked (dst, src) pair list. `jit_sync_postexit` was
checked and cannot bail - it returns 0 or 2 - so its `exit_pc` is always
a re-raise, never a re-run.

**JIT-DERIVED, NEVER A BYTECODE FACT.** The pattern is recognized from
the instruction sequence at emit time, so the interpreter is untouched,
no myv version moves, and a hostile image cannot assert it - #137's
layering has no way to verify a claim like "this slot is an argument",
and a false one would leave a reference in a slot nobody owns.

**THE GATE I DID NOT ANTICIPATE, and the test that found it.** The
coercing arm widens bool->int / int->float **in the argument temp**,
and its soundness note reads "emit_args_range gives every argument a
FRESH temp" - which a fused argument is not, so widening one would write
the CALLER'S VARIABLE. Declining that arm for fused arguments took the
inline widening away from every named-local argument and
`jit_bind_widen` failed at once (its widening argument is a plain int
loop counter). The fix: fuse ONLY `ref_slots` members. A reference at a
numeric parameter already declined there, so the arm behaves exactly as
before - and it is the right VALUE gate independently, since a trivial
argument's staging move is already the inline two-store path.

Other gates: a named local (never a temp - lever A's forwarding and C5's
release picker are where a write may legitimately be SKIPPED; DEFENSIVE,
not sabotage-falsified, and the code says so); not register-pinned
(N5/C2a) or type-elided (C3/C4a-i), whose memory is stale; the source
outside the run; CachedCallV excluded (`jit_cached_probe` builds the
pure-cache key from the run BEFORE the push); the native-direct CallV
excluded (it reads the run from C++ with no decline join).

Lever: `MYLANG_JIT_OFF=argfuse`. Counters: `g_jit_arg_inplace` (bumped
by the EMITTED copy loop, so the helper tier cannot satisfy it) and
`g_jit_arg_stage` (the cold arm - the path where a mistake is a
use-after-free rather than a wrong answer, so it gets its own proof).

SABOTAGE, all watched: removing the ref_slots gate fails `jit_bind_widen`
AND arg_inplace_shapes' int-local decline; removing the cold-arm
materialisation fails the ref-arg bind test and then ABORTS the suite,
and the cold shape alone gives InternalErrorEx; removing the named-local
gate fails NOTHING (recorded as defensive).

---

## NET 2 - the DETERMINISTIC EVENT SWEEP for the no-record tier (2026-08-13)

Not an optimization: a TEST NET for one that already shipped. The G1
no-record tier (default-ON since 291c2fc) does not write a `VmCallRec`
for a call it can REBUILD later, and the rebuild runs only on the rare
paths that ask - an unwind step, a backtrace frame. So its correctness
was exercised wherever a corpus happens to throw, and nowhere else.

`MYLANG_RECON_AT=N` (or `g_norec_recon_at` from a test) forces
`norec_recon_probe` at the Nth CALL EVENT - every emitted M5b push is
one - and the driver `tests/norec_sweep.py` walks N over a program's
whole event count, one process per N. SQLite's fail-the-Nth-malloc,
applied to frame reconstruction.

WHAT THE PROBE CHECKS, and why it is not a duplicate of Nets 1/1b.
Those verify chain INTEGRITY (site association, anchor links, the
descriptor and window chains) at every push. The probe rebuilds, from
hardware + baked constants only, the values the record-less unwind
INSTALLS, and adds three things they do not have:
 - the CUMULATIVE slot-stack arithmetic: each frame's
   `seg_top_before + nslots` must land exactly on the next frame down's
   `seg_top_before`, the topmost on the live `seg->top`;
 - `nslots` against the CHUNK totals - the derivation the record-less
   un-accounting uses instead of reading the record;
 - a chain walk in the PRODUCTION configuration. Net 1's walk is gated
   `!jit_norec_on()`, so in the shipping mode nothing traverses the
   chain at all; the probe does (termination, site resolution, RA
   agreement, ascending links), which is the half that can be asserted
   without a record to read.

Two modes: `MYLANG_JIT_OFF=norec` keeps records, so the rebuild is
compared field-for-field (the oracle); the default is record-less and
walks the real mixed chain. The probe is READ-ONLY - the sweep compares
every run's stdout and exit code against a probe-free baseline, so a
probe that PERTURBS a program fails even when nothing mismatches.

SABOTAGE, watched failing: corrupt the emitted push's `seg_top_before`
store by one (`add r11,1` around the store in the record fill). `-rt`
stays GREEN at 1907/1907, `corpus_diff` green at 15/15, and the program
prints the right answer - the slot stack merely leaks one slot per call.
The sweep fails at N=1: "live seg->top != top record's seg_top_before +
nslots". That is the whole argument for the net: the existing tree does
not look.

TWO VACUITY TRAPS, both hit while writing it and both now guarded. The
walk was first driven by the RECORD COUNT, which made it walk zero
frames in production (where most frames have no record) - the mode that
ships was silently uncovered, visible only as "0 frames, production" in
the report; it is driven by the native chain now. And the in-suite seed
first reported "0 probes fired", because the event counter is
process-global and -rt has already made thousands of calls by the time
that entry runs - it rebases per run. The seed asserts FRAMES walked,
not just probes fired, for the same reason.

Measured: 1212 forced reconstructions over the default corpus
(tests/functional + samples) in ~5.5 min, debug+ASan. Corpus reach is
5 of 19 programs - most functional tests make no emitted sync calls -
so `tests/functional/09_norec_deep_calls.my` was added to give the sweep
a TALL chain (175 call events; mutual recursion inside a loop, which is
what defeats the two shape-eaters `jit_norec_shadow` documents).

STILL OPEN from the plan's testing arc: Net 3 (exhaustive small-scope
enumeration) and Net 4 (the GCOV coverage gate).

---

## NET 3 - EXHAUSTIVE SMALL-SCOPE ENUMERATION (2026-08-13)

The second of the no-record tier's unbuilt nets. NOT a fuzzer:
`tests/norec_enum.py` emits EVERY program in a bounded shape space and
runs each through four engine configurations, comparing stdout, stderr
and exit status BYTE-FOR-BYTE.

    depth        1..4 chained functions
    frame kind   per level: plain / try / try-finally / dict-iter
    terminal     return int / return float / throw
    catch level  for a throw: caught at level j, for every j, or not

2272 programs, 9088 engine runs, ~1.3 min. RULE 2 is the spec, and an
uncaught throw's rendered BACKTRACE is the hard consumer: it is built
from the frames the tier reconstructs.

TWO AXES SUBSTITUTED, recorded rather than silently dropped (the plan's
axis list is closed to removals). "cached-call" as a frame kind is
unreachable: a frame gets a cache key only when its callee is a pure
tree-recursive function, which cannot also be a link in an impure
chain. And no builtin captures a backtrace without throwing, so
CAPTURE is covered by every throw variant (frames are recorded as the
exception unwinds, caught or not) and RENDERING by the uncaught ones.

TIER REACH is sampled and printed, so a space that never engages the
tier reports that instead of a green zero (12 of 25 sampled at depth 4).

IT FOUND TWO REAL BUGS ON ITS FIRST RUN, neither caught by `-rt` or
`corpus_diff`:

1. **The mixed-kind ret audit abort.** A plain -> try -> plain ->
   dict-iter chain aborted `jit_ret_audit`, which read
   `act.back_rec()` as the returning frame's record - an ANCESTOR once
   record-less frames interleave. Correct in every engine, but
   VM_HARDENING is ON in CI's RELEASE lanes. Fixed separately.
2. **The catch-bind `ref_slots` gap** - the more serious one, and a
   direct consequence of #78 step D. `compute_ref_slots` derives its
   list from instruction write-dsts; deleting the interpreted
   `CatchTest` chain moved the catch binding into the RAISE PATH, so
   the slot `catch (T as e)` binds a STRUCT INSTANCE into is written
   by no opcode and never entered the list. `pop_window`'s release
   scan skipped it and the hardened re-scan aborted. Only the
   caller-catches-a-callee's-throw spelling trips it, which is why no
   corpus program ever had. Fixed by feeding `handler_sites`' bind
   slots into `compute_ref_slots`.

SABOTAGE, watched: with the `ref_slots` fix removed, `-rt` exits 0 and
`corpus_diff` exits 0 while the enumeration fails 16 of 96 at depth 2.
`tests/functional/11_catch_bind_release.my` was then added so
corpus_diff catches it too (sabotaged: exit 1, 17/18) - a cheap pin for
a shape that took an exhaustive search to find.

STILL OPEN from the plan's testing arc: Net 4, the GCOV coverage gate.

---

## NET 4 - the COVERAGE RATCHET for the no-record tier (2026-08-13)

The last of the tier's unbuilt nets. `tests/norec_coverage.py` reads
gcov's JSON from the existing `-DGCOV=1` lane and reports LINE and
BRANCH coverage of the walk / reconstruction / verification surface,
per function, against the `SCOPE` list in the script.

THE FINDING THAT JUSTIFIES IT. A plain `./mylang -rt` leaves
`norec_walk_chain` at **0% - never executed**. It is gated
`!jit_norec_on()`, so the project's primary shadow oracle runs only in
SHADOW mode, which `-rt` does not select. The whole scoped surface
measured **54.8% lines / 47.0% branches** from `-rt` alone.

WHAT MOVED IT. `--run` drives a workload that reaches those paths -
`-rt`, `corpus_diff` plain AND `--levers` (which includes
`MYLANG_JIT_OFF=norec`), the Net 3 enumeration and the Net 2 sweep
(both run shadow beside production) - plus two new corpus programs
written for coverage the corpus did not have:

  tests/functional/12_deep_switch.my   recursion past the SYNC DEPTH CAP,
                                       the only thing that drives the
                                       -3/switch materializer

That one alone took `norec_materialize_shadow` from **7.2% to 66.7%**
lines and `norec_switch_retarget` to 98.3%: nothing else in the corpus
recursed past the cap (32 in the sanitizer lanes). Totals now
**77.4% lines / 62.6% branches**.

    norec_walk_chain           0.0% ->  71.1% lines,  50.0% branches
    norec_materialize_shadow   7.2% ->  66.7% lines,  52.5% branches
    norec_recon_probe         39.0% ->  71.4% lines,  58.3% branches
    jit_norec_push_verify     46.4% ->  60.9% lines,  57.0% branches

EXEMPTIONS LIVE IN THE SOURCE, as a trailing
`/* NOREC-COV-EXEMPT: reason */`. Keeping them beside the code means
they cannot rot when line numbers shift, and the next person to edit
the line sees the claim they have to keep true. A marker that is no
longer needed is reported STALE, so the list cannot quietly accumulate.
gcc's exception edges are excluded by default: it emits one on every
call that can unwind, and counting them would make the target
unreachable for reasons that have nothing to do with testing.

**THE 100% GOAL IS NOT MET, and this is the honest accounting.** ~335
scoped lines/branches remain uncovered. They are dominated by:
 - `norec_fail`'s ABORT arms and the `ML_VM_CHECK` failure edges - by
   construction only reachable by crashing the process;
 - loop backstops like `if (guard > 100000)`, which exist precisely so
   that a corrupted chain terminates and which no correct run takes;
 - paths needing a runtime coincidence the corpus does not yet build
   (a call exactly at a `SEG_SLOTS` boundary; a reconstruction spanning
   a segment boundary AND a native-stack growth - two of the three
   cases the plan enumerated explicitly as not covered by the depth
   bound).
Marking 335 items would be the "silently exempt" failure the design
forbids - a marker with a hand-waved reason is worse than no marker -
so instead CI pins the CURRENT floor (`--min-lines 70
--min-branches 55`) and the absolute target stays tracked work. The
floor is well above the `-rt`-only 54.8/47.0, so a change that stops
the shadow workload running fails the gate immediately.

REMAINING, in the order that would pay: cover the two enumerated
boundary cases (a call at a segment boundary, and a reconstruction
spanning a boundary plus stack growth); then annotate the abort arms,
which is mechanical once the reachable set is genuinely exhausted.

---

## THE THREE CASES NET 3's DEPTH BOUND DOES NOT COVER (2026-08-13)

`plans/archived/g1-no-record-tier.md` enumerates them explicitly, and
says why the enumeration cannot produce them: they are reached by
DEPTH, not by shape.

    a call exactly at a SEG_SLOTS segment boundary
    recursion deep enough to GROW the native stack
    a reconstruction spanning both

All three are now covered by ONE check, `norec_segment_boundary`
(tests.cpp): 12000 levels of mutual recursion that throws at the bottom
and catches at the top.

WHY A BOUNDARY MATTERS. The slot stack is segmented at
`SEG_SLOTS = 16 * 1024`. An emitted push DECLINES across a boundary -
its fit test sends the call to C++ - so a boundary is precisely where
record-ful and record-less frames interleave, and where
`seg_top_before` / `parent_seg` arithmetic has to be right. The throw
makes the unwind SPAN the boundaries rather than merely reach them.

MEASURED, and the two lanes differ on purpose:

    lane                        seg_advance   sync_depth_max
    rel-hard (stack armed)          2             12001
    debug + ASan (stack off)        2                32

`ML_NSTACK_OFF` is set under sanitizers, where the sync cap is 32 and
everything past it runs interpreted-flat. So the BOUNDARY case is
covered in every lane and the NATIVE-STACK case in the non-sanitized
ones - which is the right split, since a 12001-deep native stack is
exactly what the sanitizer lanes are configured not to build.

TWO NEW COUNTERS make this measurable at all, because depth leaves no
other trace: `g_vm_seg_advance` (real advances only - the first segment
allocation is excluded, and counting it was the first version's bug,
reading 1 at every depth) and `g_jit_sync_depth_max`. Both TESTS-only,
both in `MYLANG_JITSTATS`.

IT IS AN extra_check, NOT A `tests` ENTRY, and that is forced: the
differential reruns every `tests` entry in the TREE-WALKER, which
recurses on the C stack and overflows at this depth (documented; ASan
frames are huge). Capping the depth to suit it would drop below the
boundary the check exists to cross - the first attempt put the program
in `tests/functional/` and `corpus_diff` went 19/20 with a tw
stack-overflow. A segment boundary is a slot-stack concept the
tree-walker does not have, so the meaningful comparison is across the
VM configurations, which the check runs itself (jit off and on).

SABOTAGE, watched: dropping the depth from 12000 to 100 fails it with
"crossed 0 segment boundaries" rather than passing as a merely-deep
test. That assertion is the load-bearing half - without it a future
frame-layout change could quietly stop reaching SEG_SLOTS and leave
something that proves nothing.

## H6 (2026-08-13): the boxed inline tier gets its FLOAT arm - and the
## reach measurement that had to come first

#60 gave the boxed (dyn) arithmetic ops an INLINE fast tier: guard both
operand type words against `t_int`, do the payload arithmetic at the
site, store. Its own comment named what it left behind - *"ANY other
shape (float/bool/string/mixed operand, a throwing aop) falls to the
EXACT helper path below"* - and the float half was never built. This is
that half.

**THE REACH MEASUREMENT CAME FIRST, AND IT SAID ZERO.** The plan sized
H6 off 78_typed_param_call's float accumulate; H4 has since typed that
call result, so the motivation was stale. A new classifier on the slow
helpers (`g_jit_boxed_slow` + `_f` + `_m`, TESTS-only, in
`MYLANG_JITSTATS`) counted the whole corpus: **`boxed_slow_f` is 0 in
every one of bench/my + samples**. The declines that exist are string
`+` (17, 28, strloop) and a handful in 39/75/primes. A two-line probe,
on the other hand, produced **8,000,000** float-float declines - so the
shape is real, common in user code, and simply absent from the corpus.
That is a corpus gap, not a reason to skip the work (the sibling-case
rule), so **79_dyn_float** was added: the float twin of 66_dyn_foreach.

WHAT SHIPPED - `boxed_float_arm` (jit.cpp) decides eligibility, and it
has TWO callers on purpose: the emit AND `run_has_float`, which is what
puts `t_float` in r8 at fragment entry. The arm's guard and its store
both read r8, so a predicate that drifted from `run_has_float` would
leave it holding garbage; one function cannot drift from itself.

  * BOTH operands must be PROVABLY float - a float LITERAL, or a slot
    the emitted guard compares against t_float. This is CORRECTNESS, not
    conservatism: an int-int pair must produce an INT, and the int arm
    declines for its own reasons (a div edge case, a non-int literal),
    so an arm that promoted whatever it was handed would turn `1 + 2`
    into 3.0. The MIXED int/float promotion is therefore still the
    helper's - enumerated, not forgotten, and `boxed_slow_m` sizes it.
  * ARITH is `+ - * /`. `%` is the libm fmod call the helper already
    makes; the bitwise ops do not exist for floats. A RUNTIME `+-0.0`
    divisor DECLINES via the sign-stripped BITS test (`movq rax, xmm1;
    shl rax, 1; jz`), which is exactly `fpclassify(rhs) == FP_ZERO` -
    the same reasoning FloatBin uses, and the reason a bare `ucomisd;
    je` is wrong (unordered sets ZF, so a NaN divisor would wrongly
    decline). A literal `+-0.0` divisor is refused at emit time.
  * COMPARES are the four ORDERING ones, via CmpFloatV's ucomisd
    operand-SWAP trick, so an unordered (NaN) compare is FALSE in all
    four. eq/noteq stay out, matching CmpFloatV's own long-standing
    decision - NaN needs PF, i.e. a second setcc and a cmov.
  * CompoundV stores PAYLOAD ONLY (the guard proved the dst already
    holds a float, so its type word is t_float and there is nothing to
    release) - the int arm's shape. BinOpV goes through
    `emit_float_store`, which handles a ref-listed dst.

READING THE TYPE WORD IS SOUND ONLY BECAUSE A BOXED OP'S SLOTS ARE NEVER
PINNED OR ELIDED, and that is enforced, not hoped: the cache scan's
`bad()` inserts into `disq` AND `disq_f` for BinOpV/CmpV/CompoundV, and
both elision sets filter on those. The int arm has always relied on it
silently; an NDEBUG-gated `ML_CHECK` now states it, so a future
reclassification fails loudly instead of reading a stale word.

MEASURED (`OPT=1 ASSERTS=0` both sides, interleaved `--baseline`, one
full suite):
 - **79_dyn_float 0.183s -> 0.036s = 0.20x wall**, my/python 0.13x;
   8M helper calls become 8M inline runs (`boxed_fastf`), `boxed_slow`
   goes to 0.
 - suite geomean cur/base 0.984x - which is 79 itself; **93 of 101
   corpus programs have byte-identical `-vd`** and the 8 that differ do
   so because of the LogV codegen fix in the previous commit, not this.

SABOTAGE, all watched failing:
 - the arm disabled -> `BinOpV inline DID NOT RUN` (the counter is
   bumped by the EMITTED code, so a value assertion could not tell the
   tiers apart);
 - the `+-0.0` bits guard removed -> the div0-decline shape's
   `caught == 20` fails;
 - the ucomisd swap removed -> the NaN shape fails;
 - the both-float operand rule dropped -> the pre-existing `var dyn d =
   runtime(2.5); var dyn r = 3 + d;` test fails on all four VM
   differential modes. Note it is NOT my own mixed shape that catches
   this: the runtime type test still declines an int-int pair, so the
   rule is load-bearing only for a LITERAL operand, which has no runtime
   guard at all. The test says so rather than claiming credit.

REMAINING SIBLING CASES, enumerated: the MIXED int/float arm (needs a
per-operand int-or-float guard plus cvtsi2sd, and must still refuse
int-int - `boxed_slow_m` says how much it would buy); float `%` (a libm
call either way); eq/noteq compares; and UnaryV, which no arm covers in
either tier.

## H7 inc 1 (2026-08-13): the unpack's STORAGE DISPATCH leaves the
## element loop - 75 is -19.4% Ir with no emitted code at all

Post-H3 the strict element unpack was still a four-deep chain -
`jit_unpack_elem -> vm_unpack_elem_body -> vm_unpack_bind_elem` (once
PER ELEMENT). The plan scoped H7 as an emitted INLINE tier, the shape
#92 (the element store) and #93 (the nested read) both got. Measuring
first said most of the cost was not where a tier would attack it.

MEASURED BEFORE ANYTHING WAS WRITTEN (callgrind, scale-1-vs-3 delta so
compile time is excluded; 5M rows per scale unit): 294.2 Ir per row on
75_indexed_unpack, of which ~222 is the unpack machinery. The split:
~86 navigation + guards, ~100 the two binds, ~18 the jit_unpack_elem
wrapper. And inside the 100, only ~44 is the intrusive_ptr traffic the
bind exists to do - the rest is a non-inlined call, a re-read of
`sub.skind()` and `sub.offset()`, and a re-derivation of the element
vector, N TIMES for a sub-array that cannot change between them.

So inc 1 hoists the dispatch instead of emitting anything: dispatch on
the storage kind ONCE, then run a kind-specialized loop over a base
pointer computed once. `vm_unpack_bind_elem` is gone - its three arms
are the three loops. The binds are untouched; this removes only the
per-element re-derivation.

Nothing in the loop can invalidate the hoist: the only writes are to
FRAME SLOTS, no user code runs, and the base frame slot owns the outer
array throughout (the same argument vm_slot_bind_value's
reference-into-storage note already relies on).

THE SIBLING WENT WITH IT: vm_multi_unpack_body's plain-bind arm had the
identical per-element dispatch, so it hoists the same way. A compound or
coercing position still needs the boxed element and keeps store().

MEASURED (callgrind Ir, `OPT=1 ASSERTS=0` both sides, steady-state per
scale unit; wall clock from one interleaved full-suite --baseline run):
 - **75_indexed_unpack -19.4% Ir, 0.83x wall** (0.059 -> 0.049s); the
   unpack machinery goes ~222 -> ~165 Ir/row, which is the plan's
   150-200 target on its own basis.
 - **73_multi_unpack -7.1% Ir** (1.04x wall, but at 0.018s it is inside
   the noise band).
 - 20_foreach_unpack -0.2% and 22_multi_assign +0.0%, both expected:
   their flat-scalar arms already ran a hoisted loop.
 - suite geomean cur/base 1.003x - flat.

THREE HOISTS, THREE SABOTAGES, ALL WATCHED FAILING - and each needed a
SLICE sub-array to be catchable at all, because a whole array has
offset 0 and the missing `+ off` is then invisible:
 - the strs arm's offset -> `slice str rows` fails;
 - the general arm's offset -> `slice general rows` fails. The FIRST
   version of that case was VACUOUS: a plain heterogeneous literal makes
   the OUTER container dyn too, so the foreach lowered to `fe.dyn.next`
   and never reached the unpack op - the sabotage did not move its value
   and the suite stayed green. `dynarray()` forces general storage while
   leaving the outer typed, which is the reachable shape.
 - vm_multi_unpack_body's `roff` -> `multi-unpack over a str slice`
   fails. Every pre-existing multi-unpack case destructures a WHOLE
   array, so none of them could catch it.

WHAT INC 2 WOULD BE, sized rather than promised: the residue is ~165
Ir/row, of which ~42 is intrusive_ptr traffic (irreducible), ~18 the
jit_unpack_elem wrapper and ~25 the navigation. An emitted tier doing
the outer/row navigation and guards inline - the #93 idiom, which
already has the layout constants and the slice/kind arms - would take
it to ~105 Ir/row. Inlining the STRING bind as well means emitting
refcount code (retain, release, the cold free branch), which is the
highest-risk category in this codebase for the ~42 it would not even
remove; the navigation half is the part worth doing.

## H8 inc 1 (2026-08-13): the depth cap IS the segment budget, so the
## live-slot counter is gone - and the depth cap gets its FIRST test

H8 in plans/top5-cpp-gap.md is "kill the seg-top/used accounting +
restore (~30-40 Ir/call)" by putting the frame window on the native
stack, and it is marked a big design step. Counting the emitted call
protocol confirms the size: **~28 instructions per call** across push and
pop - the cap test (4), the cur_seg -> segs[] -> seg* derivation (4 in
push, 3 in pop), the fit test (5), the window computation (4), the
seg->top update (2), and `used +=` / `used -=` (3). Inc 1 removes the
two that need NO fork.

**THE CAP TEST AND `used` ARE REDUNDANT.** `used` was a second running
total maintained beside every segment's own `top`, purely so
`push_window` could test `used + n > cap`. But a frame can only exceed
the cap by needing a SEGMENT the budget cannot pay for - within a
segment, the fit test `top + n <= cap_slots` already bounds it. So the
cap becomes a BUDGET (`VmActivation::room`) spent when a segment is
CREATED, and the fit test IS the cap test.

EXACT, not approximate, which is a RULE 1 requirement and not a detail:
a new segment is sized `min(max(n, SEG_SLOTS), room)`, so the LAST
segment is exactly the remaining budget and the total ever allocated is
exactly the cap - a program overflows at the same depth it did before.
The successor-reuse test was relaxed from "at least SEG_SLOTS" to "fits
this frame", because otherwise a deep/shallow/deep program would insert
and CHARGE for a fresh segment on every pass and exhaust the budget for
a depth it had already reached.

**THE ACTIVATION HOLDS ITS SEGMENT** (`cur_sg`) instead of re-deriving
`segs[cur_seg]` from a sign-extended index and an indexed load, in
EMITTED code, twice per call. `cur_seg` stays - it is what the records
store and what the parent-view restore reads - and an `ML_VM_CHECK`
pins the two together at every push and pop.

MEASURED (callgrind Ir, `OPT=1 ASSERTS=0` both sides, steady-state per
scale unit), and the ZEROES are the interesting part:
 - **10_recursion_deep -4.74% Ir, 0.93x wall**;
 - 09_fib_recursive and 08_func_call **exactly +0.00%** - not noise,
   byte-identical. MYLANG_JITSTATS says why, and it is the "prove the
   code ran" rule paying off: 10 makes **1,352,549** emitted pushes, 09
   makes **10,687** (the per-frame pure cache dedups the recursion), and
   08 makes **ZERO** (its callee is inlined away). The change is
   unreachable in two of the three benches that look like call
   benchmarks.
 - suite geomean cur/base **0.999x**, my/python 11.75x.

THE HARDENED-RELEASE LANE CAUGHT THE ONE BUG, AND ONLY IT. `cur_sg` has
to be restored on pop exactly as `cur_seg` is, and the EMITTED pop
restored only the index - so a pop that walked BACK a segment left the
pointer on the dead frame's. `-rt` under ASan, corpus_diff, the levers
matrix, clang and all four nets were GREEN; `TESTS=1 OPT=1
VM_HARDENING=1` aborted on the M5a deep-recursion test, because that is
the only configuration whose depth reaches a real segment ADVANCE (the
sanitizer lanes cap the native stack far below it). The fix is a
`parent_sg` field beside `parent_seg`, captured at the same instant, so
the two cannot disagree - plus an `ML_VM_CHECK` at every pop that says
so. Note the cost lands ONLY on record-ful calls: the steady-state Ir
for 10_recursion_deep is byte-identical before and after the fix,
because every one of its 1.35M calls takes the no-record tier and skips
the record fill entirely.

**THE DEPTH CAP HAD NO TEST AT ALL** before this - the catchable
StackOverflowEx is the whole point of the segmented slot stack ("a
clean, located error where the old per-call C-stack model segfaulted")
and nothing exercised it. It cannot be tested from `-rt`: the cap is
read ONCE per process into a static, so only a spawned binary can set
`MYLANG_VM_STACK`. Three checks in `tests/driver_checks.sh` now cover
it - a deep recursion caught by `catch (StackOverflowEx)`, an UNCAUGHT
one rendering as a located error rather than a crash, and (the
dangerous direction) a recursion that FITS still completing, since a
cap that fires too EARLY refuses a working program and nothing else in
the tree would notice.

WHAT INC 2 WOULD BE - the fork the maintainer has to settle, NOT
something to start unilaterally. The residue is the fit test (5), the
window computation (4), the seg->top pair (2) and the pop restore (2):
~13 instructions that only disappear if the window IS a stack pointer.
Four things depend on the current shape, and all four are load-bearing:
 1. **the G1 walker's reconstruction premise.** Net 2 and Net 3 verify
    the CUMULATIVE chain `seg_top_before + nslots == the frame above's
    seg_top_before`, field for field. A bump-pointer window has no
    per-frame watermark to chain, so the oracle would have to be
    re-derived before the change, not after.
 2. **StackOverflowEx** would move from a budget test to a guard
    page/limit compare - and RULE 1 says the outcome stays a thrown,
    located, CATCHABLE exception, never a signal.
 3. **helper visibility**: C++ builtins hold frame `LValue *` ACROSS
    user-code callbacks (sort's arg0 over its comparator, map/filter's
    container). A window's address must stay valid for its whole
    lifetime - which a bump pointer on a NON-relocating stack does
    satisfy, but it has to be argued, not assumed.
 4. **slot construction**: segment slots are constructed ONCE and reused
    window-over-window. A stack window must reset its slots to `none`
    somewhere; today the POP does it, so this is a move rather than a
    new cost - but it is the reason the segmented design exists and it
    has to be re-measured, not waved through.

## H8 inc 2 (2026-08-14): the slot stack's watermark is a POINTER, so
## the window IS the watermark - and the oracle was rewritten FIRST

Inc 1 removed the redundant live-slot counter. What was left in the
emitted call protocol was the conversion between a slot INDEX and an
address: the window is `slots.data() + top * sizeof(LValue)`, so the
push loaded the base, moved the index, multiplied by 48 and added -
then saved the index in the record so the pop could put it back.

`VmStackSeg` now holds `cur` and `end` as `LValue *`. The push reads
`cur` AS the window (no multiply, no base load), the new watermark is
computed once into r11 by the fit test and stored, and the pop writes
back the record's own `window` - which is the same value the deleted
`seg_top_before` held, so the field is gone rather than moved.

    ; before                            ; after
    mov  r11, [seg+top]                 imul r11, rsi, 48
    push rax                            add  r11, [seg+cur]
    lea  rax, [r11+rsi]                 cmp  r11, [seg+end]
    cmp  rax, [seg+cap]                 ja   slow
    pop  rax                            ...
    jg   slow                           mov  rdx, [seg+cur]   ; the window
    ...                                 mov  [seg+cur], r11
    mov  rdx, [seg+slots]
    mov  rax, r11
    imul rax, 48
    add  rdx, rax
    lea  rax, [r11+rsi]
    mov  [seg+top], rax
    mov  [rec+seg_top_before], r11

Nothing is spilled now: the 3-operand `imul r11, rsi, 48` needs none of
r11's old value, where the index form had to push/pop rax to build its
sum. The compare is `ja`, not `jg` - these are addresses.

**THE ORACLE WAS REWRITTEN BEFORE THE THING IT VERIFIES CHANGED**, which
is the only order that works here. The no-record tier's reconstruction
is checked by a CUMULATIVE equality over the walked frames, and it was
stated on the deleted field:

    if (R.seg_top_before + R.nslots != expect_top)     // before
    if (R.window + R.nslots != expect_cur)             // after

The two are the same predicate - the old field was set to `window -
slots.data()` in the same breath as `window` itself - and the address
form is STRICTLY STRONGER: an index equality can hold with two frames in
DIFFERENT segments, an address equality cannot.

PROVEN, not asserted, with the canary this net was built for - an
emitted push that leaks ONE SLOT per call:

    -rt                1911/1911 PASS
    corpus_diff        green
    Net 2 sweep        FAIL: "recon: live seg->cur != top record's
                              window + nslots"

Same defect, same net, through the translated assertion. (The FIRST
sabotage attempted - the walker's rebuilt window off by one slot -
aborts `-rt` outright, so it proves nothing about this check
specifically; the leaked-slot form is the one that isolates it.)

MEASURED (callgrind Ir, `OPT=1 ASSERTS=0` both sides, steady-state per
scale unit; wall from one interleaved full-suite `--baseline` run):
 - **10_recursion_deep -2.99% Ir**, and the per-call figure is exact:
   **6.0 instructions per emitted push**, matching the hand count (5
   from the sequence above, 1 from the deleted field's store). With inc
   1 the call-window work is -7.6% on that bench.
 - **WALL CLOCK FLAT** - 1.01x on the bench (0.018s, inside the spread
   its byte-identical neighbours show in the same run) and 1.004x
   geomean. This is the documented instruction-vs-time divergence, in
   the shape the guard-elision family already established: what was
   removed is a load, an imul and a store on L1-resident data, which
   retire alongside a call protocol whose real cost is memory traffic.
   Kept on the CLAUDE.md rule - a neutral wall clock is not a reason to
   revert a correct change that removes work - and reported as two
   numbers rather than one.

A COVERAGE GAP FOUND WHILE PROVING THE ABOVE, and NOT introduced by it:
corrupting the MATERIALIZER's `nr.nslots` by one (the record it
synthesizes at RAISE time for a record-less frame) is caught by nothing
- not `-rt`, not corpus_diff, not the sweep. The probe's chain check
walks CALL-time frames, and the one check that would see it compares
`nslots` against the chunk totals it was derived from, so it is
self-consistent by construction. The index form had the identical
structure. Recorded here rather than fixed, since it is a pre-existing
hole in a different path.

## CONSTANT-DIVISOR STRENGTH REDUCTION (2026-08-14): the one place where
## the instruction count was FINE and the clock was 8x off

06_if_branch runs `i % 3` twice per iteration, costs **33 instructions
per iteration** - only modestly worse than the C++ - and was **8.8x**
its twin. The reason is not in the instruction count at all: the JIT
emitted `cqo; idiv rcx` with a LITERAL divisor, and a 64-bit `idiv` is
~26-40 CYCLES and not pipelined, so two of them serialize ~60 cycles
into a body g++ finishes in ~10. g++ never emits idiv for a constant
divisor. This is the INVERSE of the divergence the guard-elision family
documents, and nothing in the usual discipline - which counts
instructions - could have found it.

THE IDENTITY, for |d| >= 2: pick the smallest shift s with
`M = floor(2^(64+s)/d) + 1` and `e = M*d - 2^(64+s)` satisfying
`e < 2^(s+1)`; then `floor(M*n / 2^(64+s))` is the floor quotient for
every |n| <= 2^63, and the `shr 63; add` tail converts floor to
MyLang's truncate-toward-zero. The bound is deliberately the
CONSERVATIVE one (|n| <= 2^63 on BOTH sides rather than the tighter
per-sign bounds a compiler uses): it costs one extra shift step for some
divisors and makes ONE condition cover the whole int64 range, INT64_MIN
included. A negative divisor negates the quotient; `n % d` is
`n - (n/d)*d`.

**THE ONE SUBTLETY, AND IT IS SILENT: `imul` IS SIGNED.** A reciprocal
with its top bit set is read as `M - 2^64`, so the high half comes out
`n` too low and the dividend must be added back. The first version
gated that on "M did not fit in 64 bits" instead of "M >= 2^63", which
is wrong for every divisor whose reciprocal lands in [2^63, 2^64) -
including **3**. The effect: `3/3 == 0`, `7/3 == -1`, while every
REMAINDER stayed correct (the mod path recomputes from its own
quotient). The engine differential caught it on the first run.

**THE SIBLING-CASE GAP, caught the same way.** Wired into the generic
`IntBin` alone, the reduction reached 06_if_branch ZERO times:
`specialize_arith_ops` rewrites `i % <lit>` into **IntModRI**, which has
its own emit site, as does the #9 fusion **IntAddModRI**. All three
share `div_magic` + `emit_div_magic` + `Emitter::bump_divmagic` so they
cannot drift.

MEASURED (interleaved full-suite `--baseline`, `OPT=1 ASSERTS=0` both
sides). **26 benches improved, suite geomean cur/base 0.918x** - the
whole suite 1.09x faster - and my/python **11.70x -> 13.23x**:

    06_if_branch      0.34x     53_collatz        0.58x
    03_int_arith      0.57x     71_exc_no_throw   0.63x
    07_nested_loops   0.65x     22_multi_assign   0.65x
    08_func_call      0.66x     72_exc_finally    0.66x
    48_const_fold     0.67x     49_autoconst_fold 0.69x
    51_purefunc_fold  0.70x     68_nested         0.72x
    12_higher_order   0.74x     ... 13 more at 0.88-0.96x

The 1.04-1.11x readings on 18/21/35/43 are benches whose hot loops have
no constant divisor; they sit in the run-to-run band their byte-identical
neighbours show.

TWO SABOTAGES, WATCHED - AND THEY ARE CAUGHT BY DIFFERENT NETS, which is
the point:
 - the `needs_add` bound put back to the wrong test -> `-rt` fails 4 and
   corpus_diff fails;
 - the truncate-toward-zero tail deleted -> **`-rt` PASSES 1912/1912**
   and only corpus_diff fails. Floor and truncation agree for every
   NON-NEGATIVE dividend, and the `-rt` reach cases are all
   non-negative. The exhaustive value sweep lives in
   `tests/functional/14_div_magic.my` (17 divisors x 40 dividends,
   both signs, both INT64 extremes, powers of two, a 31-bit prime,
   negative divisors) precisely so the tree-walker's C++ `/` and `%`
   are the oracle. A reach test alone would have shipped this.

## THE BUILTIN-CALLBACK ENTRY: `VmInvoker::call` (2026-08-14)

Not an emitter change - the VM's callback path - but it lives here
because it added two `MYLANG_JITSTATS` counters and because its
REJECTED half is a trap worth reading before touching this area.

`VmInvoker::call(args...)` (vm.h) is now the ONE entry every
higher-order builtin uses. It takes the callback's arguments in
whatever C++ types the builtin holds them in, boxes each EXACTLY ONCE
into the argv, and picks the tier itself (the prepared window, or
`eval_func` when there is no activation to run a boundary frame on).
The five sites - sort's comparator, find's key, `make_array`,
`make_dict`, `map`/`filter` - each spelled that ladder out by hand,
and each spelled it differently.

The win is the double boxing it removes, not the tidy-up: `sort`
reached its invoker through a `cmp2(EvalValue, EvalValue)` lambda
shared by the five storage arms, so ONE flat-int comparison built two
EvalValues for `cmp2`'s parameters and then COPIED them into a
two-element argv - four constructions. Taking the raw `int_type`s
builds the argv directly: two.

MEASURED (`OPT=1 ASSERTS=0` both sides, interleaved `--baseline`):
**34_sort_custom_cmp -16.2% instructions (893.6M -> 748.9M) and 0.89x
wall clock; 12_higher_order 0.87x; full suite geomean 1.002x**, with
33_sort_ints - the same sort with NO comparator - flat, as it must be.

**⛔ THE REJECTED HALF, so it is not built a third time: binding the
raw scalar STRAIGHT into the callee's window slot**, skipping the argv
entirely. It was built in FOUR shapes - bind inlined at the site; the
same with the body shared through a file-local ALWAYS-INLINE helper;
the same plus a non-inlinable body; and the whole typed entry
out-of-line - and every one measured **1.20x-1.22x SLOWER** on
34_sort_custom_cmp while winning **every metric a simulator models**:
-28.2% instructions, -28% branches (122.1M -> 88.3M), D1 misses
464,030 -> 463,098 and LL 49,031 -> 49,043 (identical), mispredicts
+2.5%. That combination is this codebase's known front-end/code-layout
signature (the same one behind the vm_dispatch layout tax), and this
box is WSL2 with no PMU, so it cannot be measured directly here.

THE CONTROL THAT SETTLED IT, and it inverted the diagnosis: the SAME
refactored source with the typed path disabled at compile time
measured **0.90x**. So the refactor was always the win and the typed
bind always the loss - the first attempt summed them and read the sum
as one result.

REACH WAS PROVEN, not assumed: `MYLANG_JITSTATS` now reports
**`cb_prepared` / `cb_fallback`**, so "which entry did this program's
callback elements actually take" is answerable of a real program
instead of only from inside `-rt`. With the typed bind in, 34 took it
2,820,290 of 2,820,290 comparisons - so none of the above is a
measurement of dead code.

The `-rt` net is `invoker_call_tiers`, which measures each shape on
its OWN counters (both reset first) across BOTH engines - the VM cases
must take the prepared entry, the tree-walker cases the fallback - and
asserts every case's result as well as its counter. Sabotage watched:
forcing the fallback arm always fails it at the first VM case.

## #94 THE BORROW BIND: a non-escaping reference argument takes no retain

The consumer the #93 parameter escape analysis was built for. When the
analysis proves the reference bound to a parameter cannot still be
reachable after the call returns, the callee slot takes a RAW BIT-COPY
of the caller's slot and skips both halves of the refcount pair - the
retain at the bind, and the release at the frame pop. The caller's slot
owns the reference for the whole of the synchronous call, which is the
entire soundness argument.

MEASURED (`OPT=1 ASSERTS=0` both sides, interleaved `--baseline`):
**76_funcval_dispatch -13.48% instructions** (601.0M -> 520.0M, i.e.
**-81 Ir per call** over its 1,000,000 calls) and **0.88x wall clock**.
Suite geomean cur/base **1.008x** - flat. Two benches read +10% and
+16% on the wall clock and BOTH are **+0.01% on callgrind**, so they
are timing noise, not a regression; that check is why the run was not
repeated.

REACH IS PROVEN, not assumed: `MYLANG_JITSTATS` reports **`arg_borrow`**
(the retain actually skipped) and **`arg_borrow_slice`** (a parameter
the analysis cleared whose VALUE declined). Bench 76 reports
`arg_borrow 1000000` - one per call, so none of the above measures dead
code. Two counters rather than one because a single total cannot
distinguish "the tier ran" from "the tier was reachable and every value
declined", which is this project's standing vacuous-test trap.

THE DECISION IS ONE FUNCTION, `vm_bind_arg` (vm.cpp), shared by the C++
`fast_bind` and the emitted push's reference arm. The slot records the
answer in `LValue::borrowed`, which lives in `is_const`'s tail padding -
the slot did not grow, and the emitter's 48-byte stride static_assert is
what enforces that.

FOUR THINGS A FUTURE EDITOR MUST NOT SOFTEN, each watched failing:

- **Only a REFERENCE is borrowed** (`t >= t_str`). The release scan
  skips a trivial slot - correctly, there is nothing to release - so a
  borrowed int keeps its flag set FOREVER, and the next call to reuse
  that window slot rebinds over a slot still marked borrowed. This is
  not hypothetical: it fired on the first run, in **ackermann**, whose
  un-annotated TEMPLATE parameter is not `binds_scalar` (so the analysis
  claims it) but holds an int at run time. Caught by `rebind`'s
  ML_CHECK, which the step-1 commit had added as "proven redundant".
  It was not redundant.
- **Never a SLICE.** A slice registers itself in its parent's
  live-slices set when COPIED, and an element write to the parent
  detaches every registered view in place (`clone_aliased_slices`). A
  borrowed view is in no such set, so the write leaves it reading
  storage the detach just handed to somebody else - and freed outright,
  if the caller's slot held the last reference. Every OTHER reference
  type's copy is a plain retain with no registration, which is exactly
  what makes the bit-copy symmetric with the abandon at the pop.
- **`LValue::frame_release()` is THE ONE release point.** Seven scans
  open-coded `lv = LValue()` (pop_window's two arms, three no-record
  raise/return paths, the callback window pop, the JIT's cold
  `jit_release_slot`). A borrow makes the decision PER SLOT, so they
  were routed through one method FIRST, in a separate commit, while
  nothing set the flag and the change was provably inert.
- **In the emitted push, the slot zeroing MOVED ABOVE the copy loop.**
  The qword at +40 covers `container_idx` AND both flag bytes, so
  zeroing it after the binds wiped the `borrowed` flag the reference arm
  had just set. A use-after-free from a store that reads as tidy-up -
  found by reading the emitted sequence, not by a test.

THE EMITTED PUSH READS THE BIT FROM THE DESCRIPTOR rather than baking
it, because its callee is an inline CACHE, not a compile-time constant
(`JitPushLayout::desc_noescape`; four instructions on a path that is
already a call). The C++ path reads it the same way, so the two cannot
drift.

### The `-rt` net, after a second pass over the three gaps above

`jit_borrow_arg_shapes` grew from four cases to six, because the first
version covered the mechanism and not the three hazards that actually
bit. All six are watched failing:

| rule deleted | caught by |
|---|---|
| the SLICE exclusion | the #94 test, on a VALUE |
| the SCALAR rule (`t >= t_str`) | `borrow_from`'s ML_CHECK |
| the analysis's answer (borrow everything) | `borrow_from`'s ML_CHECK |
| `frame_release` honours the flag | ASan use-after-free |
| the zeroing moved back after the copy loop | ASan use-after-free |
| the per-argument index shift | the #94 test, on an exact count |

Three of those six were covered by NOTHING when the tier landed, and two
of the new cases needed a shape the obvious spelling does not produce:

- **The SCALAR case was vacuous twice, in two different ways.** Written
  `func addk(int a, ...)` the annotation makes `binds_scalar()` true, so
  the ANALYSIS skips the parameter and the runtime rule is never
  consulted. Written `dyn a` with a body that so much as copies `a`, the
  scan clears the bit for the non-base read - and a `dyn` parameter that
  only ever receives ints picks up `proven_type = i` from C3 and becomes
  `binds_scalar` anyway. The shape that works is a parameter the body
  NEVER READS (what a fixed-signature callback looks like), **with the
  JIT OFF**: the emitted push's scalar arm raw-copies without calling the
  helper, so only the C++ bind path can reach the rule - which is exactly
  where ackermann's recursion lives. With the JIT on the same program
  bumps twice, not 120 times.
- **The per-POSITION case is the only one that reads the index shift.**
  Every other case has both parameters agreeing, so a push testing bit 0
  for EVERY argument satisfies all of them - measured: that sabotage
  SURVIVED the five-case version and is caught by the sixth. It needs one
  parameter claimed and one not IN THE SAME CALL, which means the second
  must escape WITHOUT poisoning the function - a callee that RETURNS it,
  since a global write would clear the whole mask instead.

The escaping and polymorphic cases are written to fail as real
use-after-frees rather than counter checks: the callee drops the
caller's last reference mid-call and reads the parameter afterwards, so
ASan is the detector and the value is the second net.

STILL UNBUILT, in reach order: the emitted INLINE borrow arm (the call
itself is still paid - the bit test and the slice test would move into
emitted code, jumping to the raw-copy path already there); the builtin
CALLBACK bind paths (`argv[i]` - sort's comparator, map/filter/
make_dict); and the tree-walker's `do_func_bind_params`.

### Step 3 - THE EMITTED INLINE BORROW ARM: BUILT, MEASURED, REVERTED

The helper call removed by testing both conditions in emitted code. It
worked and was proven to run (`g_jit_arg_borrow`, bumped by generated
code only, read **999,999 of bench 76's 1,000,000 calls**), and it was
**reverted the same day**: 76_funcval_dispatch **-3.27% instructions**
for **1.00x WALL CLOCK**, while 10_recursion_deep (+1.44% Ir, 1.05x) and
63_closures (+0.48%, 1.07x) - neither of which ever borrows - paid for
the emitted bytes. Suite geomean 1.003x.

The removed call was a predicted jump to an I-cache-resident helper, so
it retired nearly free: **the guard-elision entry above, arrived at
independently a second time.** The full record - the emitted sequence,
the four conditions that would make it worth rebuilding, the step-by-step
re-introduction, and the two silent emitter traps it cost a debugging
cycle on - is `plans/archived/inline-borrow-arm.md`.

ONE piece of it stayed: `Emitter::movabs` now `ML_CHECK`s `reg < 8`. Its
REX is a bare 0x48 with no REX.B, so `movabs(R10, x)` silently assembles
`0xC2` - `ret imm16` - and the fragment returns into nothing. Its sibling
trap is documented beside it: `j32` takes the SHORT jcc opcode (0x74 for
`je`), and the near second byte 0x84 assembles `0F 94`, a SETE.

## LEVER A's WRITE-ELISION GOES PER-SITE (2026-08-15)

Lever A forwards an int result to the ADJACENT consumer in RAX and, when
the temp is provably dead after it, elides the slot write. Its own
comment lists the guards, one of which is "a REF-LISTED producer dst
keeps its write (the release semantics)" - because `store_dst`'s cold arm
is what RELEASES whatever the slot held, and skipping the store would
skip the release.

**Sound, but stated over the wrong unit.** `ref_slots` is per CHUNK and a
TEMP SLOT is reused across a whole chunk, so one unrelated temp anywhere
in `main` lists slot 4 and every int producer sharing it loses its
write-skip for the entire fragment. Meanwhile C5 (`Emitter::relok`)
already proves, PER SITE, that no reference can be in a given slot - and
where it does, `store_dst` emits no release at all. So the refusal now
reads `!listed || e.relok(fdst)`: keep the write where there are release
semantics to preserve, skip it where C5 has proved there are none.

FOUND BY CENSUS, not by inspection: counting `03_int_arith`'s emitted
loop body against a cachegrind scale-delta showed **11 data references
per iteration (3 rd + 8 wr), all of them TEMPS** - the locals `acc`, `i`,
`N` already live in r12/r13/r14 and never touch memory. One of the four
store PAIRS was provably dead (written at +923/+930, overwritten at
+1055/+1062, never read), and instrumenting the gate named the single
blocker exactly: `live_ok=1 lout=0 reflisted=1 -> skip=0`.

MEASURED. Emitted loop 63 -> 61 instructions, temp stores 8 -> 6.
**Data references: 07_nested_loops -35.84%, 03_int_arith -12.13%**;
01_while_loop, 44_primes_sqrt, 46_matrix_mult byte-flat.

**WALL CLOCK: NOTHING. Suite geomean 0.998x, every affected bench within
noise** - and the reason is the important part, because it refines this
file's own guard-elision finding. Cachegrind's D1 miss counts on
07_nested_loops are **50,139 -> 50,164, i.e. UNCHANGED**: all 3,000,000
removed data references were L1 HITS, to a slot nothing reads. A dead
store to an L1-resident line retires in the store buffer and stalls
nothing.

**So the currency is not "data references", it is CACHE MISSES and
DEPENDENCY STALLS.** "Removes memory traffic" was the rule that predicted
this change would pay; it predicted wrong, in the same direction as the
predicted-branch and predicted-call cases above. Refine it before using
it again: a win needs the removed access to MISS, or to sit on a
dependency chain something waits for.

KEPT rather than reverted, unlike #94 step 3, because the two differ on
the COST side: step 3 added ~40 emitted bytes at every call site and
charged 1.44% to benches that never used it, while this removes two
instructions per iteration and adds nothing anywhere (the largest
non-noise Ir delta across the suite is +68 instructions out of 225M on
11_closure_counter).

`g_jit_fwd_skip_rel` (JITSTATS `fwd_skip_rel`) counts the newly-admitted
case at EMIT time - an elision leaves nothing to execute, so its absence
from the emitted code IS the event, the same idiom as
`g_jit_relent_stores`. Pinned by `jit_fwd_skip_reflisted`; reverting the
`|| e.relok(fdst)` clause takes the counter to zero and fails it
(watched). **A first version of that test's comment claimed the string
literal was what listed the slot - MEASURED FALSE** (swapping it for an
int leaves the counter unchanged); the sabotage, not the shape, is what
rules out vacuity.

## #95 THE INLINE ord(s[i]) READ: 5.82 -> 0.62 ns/char, my/cpp 27x -> 2.7x

The census (plans/cpp-gap-ladder.md) ranked the corpus by
STARTUP-CORRECTED my/cpp and found 30_str_index_iterate worst at
**27.36x**, with the simplest loop of the eight: two `lea`s, ONE helper
call for the fused `ord(s[i])`, two `movabs` to reload the type-tag
singletons that call clobbered, a status test, and reloading the result
the helper had stored. **A function call per character where C++ does a
load.**

The emitted arm, gated on the layout self-check below:

    cmp  byte [rbx + payload + slice_off], 0
    jne  -> cold                    ; a SLICE
    mov  eax, [rbx + payload + len_off]
    <index -> rdx, cache-aware load_operand>
    cmp  rdx, rax
    jae  -> cold                    ; ONE unsigned compare: a negative
                                    ; index read unsigned is huge, so
                                    ; this catches BOTH neg and >= len
    mov  rax, [rbx + payload]       ; the StrObj *
    mov  rcx, [rax + strobj_data]   ; std::string's char pointer
    movzx eax, byte [rcx + rdx]
    <store_dst>

**MEASURED, and it BEAT the prediction** (which said ~1 ns and 27x ->
~4x): the loop-only figure, scale-3 minus scale-1 so startup cancels,
went **5.82 -> 0.62 ns/char against C++'s 0.23**, i.e. **my/cpp 27.36x
-> 2.7x** and a **9.4x** speedup on the loop. End-to-end the bench is
**0.30x**; the suite geomean is **0.990x** (1% faster overall) with no
regression - the two benches reading 1.16x/1.22x are +0.000%/-0.001% on
callgrind, i.e. noise.

This is the first prediction of the session that came out right, and it
is the category the cost model says wins: it removes a real CALL and its
BODY (a `get_ref<SharedStr>` type check, a `string_view` construction,
two stores through memory), not free instructions. Contrast #94 step 3,
which removed a predicted call to a hot helper that did almost nothing
and bought exactly zero.

### ⛔ THE LAYOUT SELF-CHECK, and why a static_assert cannot do it

The arm loads characters through `[StrObj + strobj_data_off]`, i.e.
`std::string::_M_p`. libstdc++ keeps that at offset 0 of the string and
VALID for both the SSO and the heap form, so one load serves every
string; libc++ does not lay its short form out that way, and this
project builds with clang and has a libc++ CI lane. A wrong offset is a
silent wrong character or a wild read, never a build error.

So the offset is derived from a real object (`SharedStr::JitProbe`, the
co-located-probe rule) and then VERIFIED at layout-init against a live
SHORT and LONG string; `str_inline_ok` gates the arm and a mismatch
leaves every `ord(s[i])` calling the helper. Reported as JITSTATS
`str_probe_ok`.

**AND THE PROBE CANNOT BECOME THE WILD READ IT PREVENTS** (maintainer's
requirement): a BOUND first - the offset plus a pointer must fit inside
the StrObj, whose size the probe hands out for exactly this - then a
FAULT GUARD (sigaction on SIGSEGV/SIGBUS + sigsetjmp/siglongjmp,
restored immediately) as the backstop. Watched: bypassing the bound and
forcing a 1 GB offset prints the warning, keeps the tier off, returns
the right answer and does not crash. **A failure WARNS on stderr** -
losing an optimization silently is how a platform ends up permanently
slower with nobody noticing.

⛔ **BUILD THE TEST STRINGS OUTSIDE THE GUARDED WINDOW.** `siglongjmp`
skips C++ destructors, so a string constructed inside it LEAKS when the
guard fires - ASan reported exactly that (56 bytes) on the first
version.

### The nets

`jit_ord_char_inline` covers one FIRING shape and three DECLINES (a
slice base, a negative index, an out-of-range index), each asserting the
value AND its own `g_jit_ord_inline` delta. Sabotage watched: deleting
the slice decline or the bounds decline fails it; forcing the tier OFF
fails it AND `jit_counter_coverage` ("g_jit_ord_inline is ZERO after the
whole suite"), which is a second independent net.

Two honest notes. **Forcing the tier ON is a VACUOUS sabotage here** -
the probe passes on this toolchain, so it changes nothing; only the
off direction is meaningful on a machine where the layout matches.
And `lever 4b`'s existing test had to start counting BOTH native tiers
(`g_jit_op_run[OrdCharV] + g_jit_ord_inline`), because the helper is
what bumps the former and the inline arm calls nothing - the same
tier-moved-under-a-counter fix the borrow needed.

## Lever A grows the SHIFT family (2026-08-16, #96)

**A whitelist that went stale, and the cost was silent** - the
AUDIT-TABLE STAGE TRAP (CLAUDE.md) in the shape that entry exists to
warn about. `jit_fwd_producer` / `jit_fwd_consumer` (jit.cpp) were
written over the B1/B2 specialized family as it stood: Add/Sub/Mul/And/
Or/Xor in their RR and RI forms, plus the two LoadElem producers.
`specialize_arith_ops` later grew **IntShlRR/RI, IntShrRR/RI** (and
IntModRI), and nothing re-audited the two lists. Neither is consulted
by anything that would notice - an unlisted op simply does not forward.

So every `t = a >> k; a = a ^ t` - a temp alive for exactly ONE
instruction - kept paying the full slot round-trip:

    mov  rax, a0
    sar  rax, 3
    mov  r11.type, rsi        ; the temp's TYPE store
    mov  r11, rax             ; the temp's PAYLOAD store
    mov  rax, a0
    mov  rcx, r11             ; ... and the reload, 2 instructions later
    xor  rax, rcx
    mov  a0, rax

Eight instructions, three of them memory, for a value that never
leaves the pair. With the shifts admitted it is five and none:

    mov  rax, a0
    sar  rax, 3
    mov  rcx, a0              ; the commutative SWAP - rax already holds t
    xor  rax, rcx
    mov  a0, rax

### Why the contract already fits them

The producer contract is "the int result is in RAX at the op's exit, and
no slow tier rejoins after writing the dst". The shifts satisfy both
verbatim: `write_slot(RAX, ...)` is the last thing either form emits,
and the ONLY slow tier - a negative register count - RAISES through
`emit_raise_convey` and leaves the fragment, so there is no
stale-RAX rejoin of the kind the LoadElem producers need their reload
for.

The consumer side needed one new arm. A shift is **not commutative**, so
a forwarded COUNT cannot use the arith family's swap: it is moved aside
into RCX before the value loads (`mov rcx, rax`), which still trades a
`mov` for a slot load. A forwarded VALUE - the common case - costs
nothing.

**IntModRI / IntAddModRI are CONSUMERS ONLY, deliberately.** Their
result is in RDX on the idiv path and in RAX on the div-magic one, so
admitting them as producers means normalising the result register
first. Their operand `a` is the first thing either emit loads, so the
consumer half is free.

### Measured (callgrind Ir, `OPT=1 ASSERTS=0` both sides)

| bench | Ir |
|---|---|
| 83_regs_int_40 | **-21.01%** |
| 80_regs_int_08 | **-17.97%** |
| 33_sort_ints | -0.46% |
| 68_nested | -0.37% |
| 45_gcd | -0.24% |
| 46_matrix_mult | +0.02% (COMPILE time: it emits two `mov` FEWER) |

Reach over bench/ + samples/: **11 programs** change - the four
`8*_regs_int_*`, plus 33_sort_ints, 34_sort_custom_cmp, 38_min_max,
45_gcd, 46_matrix_mult, 47_wordcount, 68_nested. Everything else is
byte-identical, which is the blast radius a whitelist addition should
have.

### The nets, and the three sabotages watched failing

Two `jit_fwd_deadtemp` cases, both with an EXACT count (the struct
gained `fwd_max`: a minimum alone cannot pin a DECLINE, and both cases
assert that the mod op contributes nothing).

  - producer whitelist reverted to its stale state -> `forwarded 0 <
    expected 24`;
  - consumer whitelist reverted -> `forwarded 12 < expected 18`;
  - the count-position move-aside deleted -> a **WRONG VALUE**, caught
    as a tw-vs-vm divergence rather than a counter miss.

**`g_jit_fwd` is now REPORTED by `MYLANG_JITSTATS`** (`fwd`). It was
bumped from emitted code and registered nowhere, so the one question
the counter exists to answer - does this lever reach a real program? -
could be asked only from inside `-rt`. It reads 16,000,000 on
80_regs_int_08 (2M iterations x 8 accumulators), 999,999 on
03_int_arith, 705,602 on 46_matrix_mult. This is the third counter
found unregistered (`g_jit_ord_inline` was the second); when you add
one, add its table row in the same edit.

### The sibling cases, so the map is complete

The FLOAT twin (`jit_fwd_fproducer` / `jit_fwd_fconsumer`) has no shift
to gain - MyLang's shifts are int-only - but it is the same kind of
list and is now covered by the same ratchet.
IntModRI/IntAddModRI as PRODUCERS is the one remaining int gap, and it
is a result-register normalisation, not a soundness question.

### THE RATCHET, and why four nets missed this (2026-08-16)

Asked how it escaped testing, the answer is that every net was blind
for a DIFFERENT structural reason, and only one of them is fixable by
trying harder:

  - **the differential, corpus_diff and the fuzzers cannot see it at
    all.** Forwarding changes no observable behaviour. An optimization
    that only affects SPEED has no correctness oracle - the same gap
    CLAUDE.md documents for AST transforms;
  - **`jit_counter_coverage` was satisfied.** It asks "did this lever
    run at all?", and `g_jit_fwd` was non-zero throughout because the
    add/mul shapes fire. A whole-lever counter cannot distinguish
    "runs" from "runs for 12 of 17 opcodes";
  - **`jit_fwd_deadtemp` was written FROM the whitelist.** Its three
    cases are load->mul->addstep, elem2->mul->addstep and a slow-path
    rejoin - every one drawn from opcodes already in the list. A test
    derived from a table can never find a hole in that table;
  - **no corpus program had a dense shift loop** until #96 wrote
    80_regs_int_08 for an unrelated reason.

`jit_fwd_family_coverage` is derived from the OPCODE ENUM instead. The
specialized family is a contiguous range, so it walks
`IntAddRR..FloatMulRI` and requires (1) every member to have a row and
(2) every row's claim - "forwardable" or "exempt, because ..." - to
MATCH the live predicate, in both directions.

Watched failing three ways:

    the shifts dropped from both whitelists
      -> IntShlRR producer is absent but the row says it must be
         whitelisted            (x8: four opcodes, both sides)
    a row deleted (the ACTUAL 2026 sequence - an opcode joins the
    family and nobody thinks about it)
      -> opcode #104 (IntShlRR) joined the specialized family with no
         row here - decide whether lever A forwards it, then say so
    a row claiming a bogus exemption
      -> IntXorRR producer is whitelisted but the row says a bogus
         exemption

For the ratchet to be able to ask, the whitelists had to become
answerable at the OPCODE level: `jit_fwd_op_is_producer`,
`jit_fwd_op_consumer_slots` (a MASK of forwardable operand positions,
since those differ per op), and the two float twins, all declared in
jit.h. **They are not a second copy** - the Instr-taking predicates are
built on them and apply the literal/aliasing rules on top, so a
whitelist has exactly one edit site. Verified as a pure restructuring:
emitted code byte-identical across all 94 corpus programs.

**Reuse this shape for the next opcode family that gates an
optimization.** The precondition is a family with a defined
enumeration; where one does not exist, that is the thing to build
first.

## #96: the caller-saved pin extension (r10/r11) - and what it measured

**The maintainer's objection was right and the plan's answer was wrong.**
This file (and plans/jit-registers.md) said MAX_CACHED = 4 is the limit.
Four is the ceiling for **PINNING**: a pin lives for the whole fragment,
so it must survive a helper call, so it must be callee-saved - and SysV's
callee-saved set is {RBX, RBP, R12-R15}, of which RBX is the slots base
and RBP the frame pointer the no-record tier walks. It is NOT the limit
on using the machine. Native code holds values in scratch registers and
spills only around the calls that actually occur, and the loops that
matter here make no call in the body.

`XCACHE_REGS = {r10, r11}` is the first step of that: the two GP
registers that are neither a SysV ARGUMENT register nor per-op scratch.
An argument register is written by the call site's own arg setup between
the prologue and the call, so a pin there needs every later argument to
read back out of the spill - a separate increment. rax/rcx/rdx are
written by nearly every op.

Two conditions, both checked rather than assumed:
  - `emit_call_prologue` SPILLS each caller-saved pin to its slot
    payload and `emit_call_epilogue` RELOADS it. The choke point already
    did exactly this for the caller-saved xmm float pins, and its own
    comment promised "a future pin in a caller-saved register would
    spill here". **The spill lands BEFORE the argument setup at every
    call site** - verified by reading the emitted code, and that
    ordering is what makes it correct;
  - the C1 navigation hoist OWNS r10/r11 while a loop region emits, so
    the pool takes them only when the fragment has no hoist region.

Lever: `MYLANG_JIT_OFF=xcache`. Counter: JITSTATS `xcache`, bumped by the
EMITTED fragment entry.

### Measured - and the result is the point

| bench | Ir | D refs | wall |
|---|---|---|---|
| 80_regs_int_08 | +0.00% | **-23.33%** | 1.004x |
| 81_regs_int_14 | -0.00% | - | 0.985x |
| 82_regs_int_25 | -0.00% | **-7.09%** | 0.984x |
| 83_regs_int_40 | +0.00% | **-4.39%** | 1.052x |

**Ir is flat BY CONSTRUCTION** and that is the finding. `mov rax,
[rbx+0x38]` and `mov rax, r10` are both one instruction and one uop; a
register allocator removes DATA references, not instructions. Exactly
20,000,000 D refs vanish in each row (2 accumulators x 2M iterations x
5 accesses), so the tier does precisely what it claims.

**And the wall clock does not move.** 1.004 / 0.985 / 0.984 / 1.052
across N = 8/14/25/40 - non-monotone in N, with a strictly-improving
mechanism and identical instruction counts. That is a code-LAYOUT
lottery, the effect this box has already produced at 12.5% (see the
vm_dispatch front-end note), not a cost. The 5% on the N=40 row
reproduces across three interleaved runs and has no other mechanism
available to it: fewer memory operations, byte-identical instruction
count.

⛔ **THE RULE THIS EARNS, and it is the mirror of the guard-elision
one.** That entry says an INSTRUCTION-count win can have a wall-clock
ceiling near zero. This is the other half: **a DATA-reference win with
an unchanged instruction count has a wall-clock ceiling near zero too.**
On a wide out-of-order core an L1-hitting slot round-trip retires
alongside the real work; what binds is the front end, and the front end
did not change. The two-regime model in plans/cpp-gap-ladder.md
predicted a payoff here (these loops are throughput-bound by
construction, where the isolated microbenchmark measured 3.21x) and it
was WRONG about this shape - the microbenchmark varied the memory ops
with the instruction count held DIFFERENT, which is not what an
allocator does.

**So the register is a PREREQUISITE, not the win.** The win is the
instruction the register makes removable:

    mov rax, r14      ; a0
    mov rcx, r13      ; i
    add rax, rcx
    mov r14, rax      ; a0 = ...
    mov rax, r14      ; <-- REDUNDANT: rax already holds it
    sar rax, 3

That reload is one instruction per accumulator per iteration, and it is
only removable once the value is in a register at all. It is lever A
generalised from TEMPS to LOCALS, and it is the next increment.

### The nets, and two gates that are DEFENSIVE rather than proven

`jit_xcache_pins` has four cases: six hot locals in a call-free loop
(engages), the same with a MyLang call (declines), a caller-saved pin
spilled around a HELPER call (engages, and is the shape the new
prologue/epilogue code exists for), and three hot locals (the
callee-saved four suffice, so the extension must stay out).

Two things a future reader must not mistake for proven:

  - **`jit_run_blocks_xcache` is redundant today.** `emit_sync_push_native`
    builds a call RECORD with r10/r11 as raw scratch, outside any
    prologue bracket, so a run containing a MyLang call must not spend
    them - but `pick_cached_slots` has no case for CallV/CachedCallV/
    CallValueV and its `default: return {}` already caches NOTHING in
    such a run. Watched: removing the gate changes no behaviour. It is
    kept because that default is a catch-all for UNCLASSIFIED ops, and
    the day someone classifies CallV there (reasonable - a call
    preserves callee-saved pins) the requirement becomes real and
    invisible. `jit_assert_no_volatile_pin` is the tripwire in both
    raw-scratch emitters; it is likewise unreachable today.
  - **The epilogue RELOAD is ABI-mandated but not sabotage-observable
    on this toolchain.** Deleting it keeps the whole suite green,
    because no helper on the reachable paths happens to write r10/r11
    with this gcc and this libm - tried with libm sin/cos in the loop
    and with an `asm volatile("" ::: "r10","r11")` clobber added to
    `jit_move`, neither diverges. SysV says a callee MAY clobber them,
    so the reload stays; a different toolchain turns its absence into a
    silent wrong answer. **The net that would make it observable** is a
    debug-only stub that deliberately writes garbage into r10/r11 after
    every emitted helper call - the MYLANG_JIT_COLD philosophy applied
    to the ABI. Not built.

## Lever A forwards LOCALS, not just dead temps (2026-08-16, #96)

The increment the caller-saved extension's flat wall clock pointed at: a
register is a prerequisite, the WIN is the instruction it makes
removable. Every whitelisted producer already leaves its result in RAX
and then writes the slot - so the next op's read of that same slot is a
reload of something already in the register:

    mov  r14, rax      ; a0 = a0 + i        (a PINNED local)
    mov  rax, r14      ; <- the reload, for nothing
    sar  rax, 3

    mov  a4, rax       ; a4 = a4 + i        (a MEMORY local)
    mov  rax, a4       ; <- store-to-load forwarding, ~4-5 cycles
    sar  rax, 7        ;    ON THE DEPENDENCY CHAIN

### The change is one condition moved, not added

Lever A required `fdst >= chunk.slot_count` to ARM. That restriction
belongs to the **write elision**, not to the read: `skip_write` needs
the destination provably dead, which only a temp's liveness gives.
Eliding just the READ asks far less - the write still happens, so the
slot stays current for every other reader - and it is worth as much,
because on a memory-backed local the removed reload is a
store-to-load-forwarding stall on the chain.

So the test moved down into `skip_write`, which is also where `tb` (the
temp bit index) stops being negative. **`1 << tb` for a local is UB**,
and that is the trap in doing this the lazy way; the sabotage that lets
a local reach `skip_write` fails 5 `-rt` tests.

**RAX genuinely survives `write_slot` on all four paths, and this was
verified rather than assumed**: the cached-register store is `mov cr,
rax`; the plain path is a type store through RSI plus a payload store;
the ref-listed FAST arm is two stores; and the ref-listed COLD arm calls
`jit_put_int`, which clobbers RAX - and then does `e.load(RAX,
a.payload)` UNCONDITIONALLY. That reload exists because of an earlier
bug (a `keep_rax` parameter that only lever A set, which let a
ref-listed loop counter compare garbage), and it is what makes this
increment sound for ref-listed locals too.

### Measured

Loop body of 80_regs_int_08: **92 -> 84 instructions**, exactly the 8
predicted (one per accumulator).

| bench | Ir | wall |
|---|---|---|
| 83_regs_int_40 | **-8.86%** | **0.89x** |
| 80_regs_int_08 | -7.30% | 0.99x |
| 59_bit_hash | -3.64% | - |
| 18_foreach_array | -2.78% | - |
| 07_nested_loops | -2.75% | - |
| 08_func_call | -2.23% | - |
| 03_int_arith | -1.36% | - |

28 corpus programs change and **every one is Ir-negative or flat** - no
regression anywhere. Suite geomean cur/base 0.999x, which is what a
change touching one instruction per producer-consumer pair should read.

⛔ **AND IT CONFIRMS THE CORRECTED COST MODEL RATHER THAN CONTRADICTING
IT.** The caller-saved extension removed **-23.3% of the data
references** on 80_regs_int_08 for **1.004x**; this removes **-7.30% of
the INSTRUCTIONS** on the same program and the biggest instruction cut
(83_regs_int_40, -8.86%) is the biggest wall-clock win (0.89x). Same
benchmark family, same machine, opposite outcomes - so the
discriminator really is the instruction count, not the traffic.

**A NOTE ON READING THAT BENCH TABLE.** Eight benches read 1.04-1.09x
"regressed" in the same run, and SEVEN of them are BYTE-IDENTICAL under
`-vdj` - so that run's noise floor is +-7% (39_find_builtin read 0.70x
on unchanged code). Diff the emitted code before believing a per-bench
number; the deterministic Ir above is the signal.

### The nets

`jit_fwd_deadtemp`'s two shift cases now carry a THIRD number,
`loc_exact` - how many of the forwards were LOCAL-sourced - fed by a
second emitted counter `g_jit_fwd_local` (JITSTATS `fwd_local`). Two
counters because the halves have different soundness arguments, and one
total could hide either going dark. The counts were derived from `-nj
-vd` before the change and matched exactly on the first run: case 1 goes
3 -> 5 forwards per iteration (pc4->pc5 and pc6->pc7 both produce the
local `s` and immediately read it; pc8->pc9 does NOT, since the addmod
reads `a` and `k`), case 2 goes 3 -> 4.

Watched failing: the temp restriction put back where it was
(`LOCAL-sourced 0, expected 16`), and a local allowed to take
`skip_write` (5 tests).

### The sibling case, so the map is complete

A COMPARE consumer still reloads. In 80_regs_int_08's `if ((i & 1) == 0)`:

    and  rax, rcx      ; IntAndRI -> temp
    mov  r11.type, rsi
    mov  r11, rax
    mov  rax, r11      ; <- still there

because `JumpUnlessIntCmp`/`CmpIntV` are not consumer-whitelisted. The
existing note explains why that was deliberate ("a counted loop's BOUND
temp is read every iteration - exactly the shape the liveness refuses"),
but the liveness net now exists, so it is worth re-examining - as a
separate increment, with the ratchet's row updated to match.

## #96 - the fragment RETURN states, and CHECKS, its write-back contract

The register cache lives in registers between an entry load and an exit
flush, so **any return that leaves a cached slot in a register resumes
the interpreter on a stale slot** - a silent wrong answer, not a crash.
`exit_pc` handles that automatically (it picks the flushing or the bare
epilogue per exit site). The direct returns do not.

`plans/jit-registers.md` carried a note that "12 raw `u8(0xC3)` returns
are NOT covered by exit_pc and must be enumerated". **The count was
stale in a way worth recording**: the epilogue consolidation had already
left exactly ONE `ret` in the emitter, inside `frag_ret()`. The hazard
survived in a different shape - **13 sites call `frag_ret()` directly**,
and only six flushed.

**What the other seven were resting on, unstated at every one of them:**
`pick_cached_slots`' opcode switch lists neither `CallV` nor
`CachedCallV` nor `CallValueV`, so a call falls into its
`default: return {}` and a run containing one is not cached at all.
Sound today - and **exactly the invariant this task exists to destroy**,
since the mandate is to spill live registers around a call and keep them
across it.

So the claim moved out of the reader's head and into the signature:

    enum class RetFlush {
        flushed,   /* flush_cache() was emitted on THIS path, above */
        empty,     /* nothing is cached here - ASSERTED */
        epilogue,  /* emit_epilogues only: exit_pc already chose per
                    * exit, so end-of-fragment cache state describes
                    * no particular exit and neither claim is checkable */
    };

**It ASSERTS rather than emitting the flush**, which is a deliberate
trade: emitting would reorder the flush against the
`mov rax, <sentinel>` every site places first, and byte-identical output
is the cheapest possible proof that adding a contract changed nothing.
Verified **108/108** over bench/my + samples + tests/functional.

⛔ **Two traps in that verification, both mine, both worth reusing.**
A `-vdj` dump is NOT directly comparable between two separately-linked
binaries: baked helper addresses and rel32 displacements differ under
ASLR (a naive compare reported 108/108 DIFFERING). And after masking
them, `40_math_builtins` still differed *intermittently against the same
binary* - its libm call displacement is 5 or 6 hex digits depending on
where the code page lands, so a `0x[0-9a-f]{6,}` rule and a
`call -0x...` rule race each other unless the call rule runs FIRST.

**WATCHED FAILING, with the sabotage being the literal future state:**
teach `pick_cached_slots` that `CallV`/`CachedCallV`/`CallValueV` are
cacheable, and `-rt` aborts at the propagate/switch arms of the call
path within seconds.

**Honest scope, because the distinction matters:** with `ASSERTS=0` that
same sabotage leaves `corpus_diff` GREEN (20/20). This is a guard for
where #96 is going, not a fix for a live bug. It is nonetheless stronger
than the two xcache gates recorded above, which fire on nothing - this
one fires on a reachable shape.

**And it earned its keep immediately.** The five `frag_ret` sites inside
`emit_ret_native` are `flushed`, not `empty`: that function flushes on
its FIRST line and all of its returns inherit it. A "is there a
flush_cache() within four lines above" classification got all five
wrong, and the assertion caught it on the first run.

## #96 - the all-slot LIVE RANGES (the allocator's input)

`jit_slot_liveness` (codegen.h/.cpp) is the backward liveness an
allocator needs, and `jit_next_use` is its spill-victim ranking. Three
questions it answers: may a register holding slot s be dropped without a
write-back (is s dead), what must be written back at an exit (the
live-out set there), and may one register serve two slots (disjoint
ranges).

**ONE FIXPOINT, TWO WRAPPERS.** `jit_fwd_info` (lever A's temps, one
64-bit word) and `jit_slot_liveness` (every slot, `words` words) both
call `jit_liveness_core`, so `visit_use_def`, `visit_pc_fields` and the
handler absorption are read from exactly one place. A second liveness
would be the audit-table trap with the worst possible failure mode - a
liveness that is wrongly CONSERVATIVE loses an optimization and says
nothing at all.

It also removes a cliff nobody was watching: the one-word mask made
`jit_fwd_info` `return false` whenever `n_temps > 64`, silently
downgrading C4a-i, C3 inc 3 and C5 to locals-only on any chunk that
large. The general form has no width limit.

**`jit_next_use` is deliberately a SEPARATE function because it is a
HEURISTIC.** It scans backward in pc order, and pc order is not
execution order: across a back edge a slot used at the top of a loop is
next used almost immediately, while this reports "not again in this
run". That is fine for choosing an eviction victim - the cost is a
reload, never a wrong answer - and would be a bug in anything that must
be correct. Keeping them apart, with the reason written at both, is what
stops the next reader reaching for the cheap one.

Emitted code byte-identical on all 108 corpus programs, so the widening
is a pure analysis addition.

### ⛔ THE ORACLE THAT SHARED ITS SUBJECT (watched failing)

The natural test is "the new all-slot analysis must agree with the old
temps-only one on every temp" - an independent computation of the same
fact. **It is not independent, and the widening is exactly what made it
stop being so:** both are now wrappers over one core, so a bug in the
core appears on both sides and cancels.

Proven, not assumed. Four sabotages were run against the test:

| sabotage | caught by |
|---|---|
| single-word packing (`b/64` dropped) | the dataflow equation |
| drop the handler absorption | the dataflow equation |
| `next_use` forgets the use | the next-use contract |
| **unaudited op contributes NOTHING live** | **nothing - green** |

The one that escaped is the dangerous one: too-little-live is what lets
an allocator drop a value that is still read. What catches it is a check
derived from the written CONTRACT rather than from another run of the
code - *an op `visit_use_def` does not know leaves every covered slot
live-in* - and that needed a program containing one of the **35
unaudited opcodes** (an array element store; `StoreElemInt` is a
barrier). The test counts them and FAILS VACUOUS if it saw none, which
it did on the first attempt.

**The general lesson, worth more than this analysis: a test whose ORACLE
SHARES THE IMPLEMENTATION UNDER TEST proves only that the shared part is
self-consistent.** Same family as "a test derived from a table can never
find a hole in that table" - and the refactor that unifies two
implementations is precisely the moment a cross-check between them stops
being evidence. Re-derive at least one check from the SPEC.

## #96 - ONE EPILOGUE PER CACHE STATE (and the barrier bug it exposed)

**Why.** Every exit is `mov eax, pc; jmp <epilogue>` - a constant 10
bytes - because inlining the tail made exits grow with the pool (56
bytes of flush at four pins) until the short jcc that hops over an exit
ran out of displacement. That sharing works only while the register
cache is FRAGMENT-CONSTANT: one flushing epilogue can write back exactly
one state. **The allocator's whole point is to change a register's
occupant mid-run**, so the exit must carry the state it was emitted
under.

`exit_pc` now interns the current `(cache, fcache, tflush)` and records
its index; `emit_epilogues` emits one epilogue per distinct state that
has an exit, installing that state around `flush_cache()`. States are
ordered non-empty-first then first-seen, which reproduces the old
flush-then-bare order exactly.

### ⛔ IT WAS NOT THE PURE REFACTOR IT LOOKED LIKE - a latent bug fell out

The old test was
`cache.empty() && fcache.empty() && tflush.empty() ? epi_bare : epi_flush`,
and the barrier path - which EMPTIES the cache across a barrier'd op's
emission, precisely so that op's exit writes nothing back - cleared
`cache` and `fcache` and **not `tflush`**.

So in any fragment where C3 type-elision was active, a barrier'd exit
read as "something is cached", took the FLUSHING epilogue, and that
epilogue flushes the emitter's FINAL (restored, full) cache. It wrote
the pre-call register values over slots the helper had just written -
the exact clobber the barrier comment says must never happen, defeated
by a third vector nobody re-read when C3 added it.

The fix is one line of intent restored (`saved_tflush = std::move(
e.tflush); e.tflush.clear();`), and it is sound because the pre-op
`flush_cache()` has already stamped those types: at a barrier'd exit
nothing is owed. Barrier'd exits now take a genuinely bare epilogue.

**Blast radius, characterised rather than asserted:** 57 of 108 corpus
programs change, and a shape check over every changed line confirms each
is either a retargeted `jmp` or part of the added bare epilogue (relay
store / stack teardown / pops / ret / a `.type` restore). Nothing else
in the emitted code moves.

**Honest scope: I could not turn it into a failing program.** It needs a
barrier'd op that WRITES a pinned int-scalar slot and then exits before
the reload; the corpus, `-rt`, `corpus_diff` (plain, `--levers`,
`--cold`), `nested_fuzz` and the Net 3 enumeration are green both ways.
So this is a latent bug closed by making the code match its written
intent, not a reproduced defect - recorded that way deliberately, like
#96's `RetFlush` guard.

**The pattern, since this is now the third one in this task:** a
predicate that enumerates "everything that can be cached" is an audited
table wearing an `&&`. `tflush` joined the flush and the barrier had no
reason to know. When you add a fourth cache vector, grep for every site
that tests all three.

### The follow-up: the enumeration now lives in ONE place

Explaining the bug above made the fix obvious. The family
`{cache, fcache, tflush}` is now listed only in four adjacent `Emitter`
members - `snapshot_cache()`, `restore_cache()`, `clear_cache_state()`
and `cache_live()` - and every site that asks about the family as a
whole goes through them: `frag_ret`'s `empty` contract, the barrier's
`brk` guard, the barrier's clear and its restore.

`clear_cache_state()` ML_CHECKs itself against `cache_live()`, which is
the mechanical net: a vector added to one and forgotten in the other
aborts by name instead of silently corrupting a slot. **Watched failing
by reintroducing the ORIGINAL defect** (delete `tflush.clear()`):
`-rt` aborts immediately with

    Assertion `(!cache_live()) && ("clear_cache_state() left something
    live: a cache vector was added to cache_live() but not here")'

Verified as a pure restructuring: emitted code byte-identical on all 108
corpus programs (`scripts/vdjcmp.sh`).

**Two tools came out of this session and now live in `scripts/`**, since
both were re-derived from scratch more than once:
`scripts/vdjcmp.sh` (compare two binaries' emitted native code across
the corpus - the oracle for "this refactor changed nothing", with the
four normalisation traps recorded in its header) and
`scripts/sabotage.sh` (apply a defect, rebuild, run a check, always
restore; exit 1 means the check PASSED, i.e. the test is blind).

## #96 - r9 joins the pool (6 -> 7), and the measurement that redirected it

> ⛔⛔ **THE r9 HALF OF THIS ENTRY WAS WRONG AND WAS REVERTED ON
> 2026-08-17. r9 was NEVER safe as a pin, and it shipped a WRONG ANSWER
> for a day.** The measurement below (sharing has zero reach) stands;
> the conclusion "so widen the pool, and r9 is the cheap one" did not.
> Read *#96 - r9 was never safe* at the end of this file before touching
> `XCACHE_ORDER`.


**The ceiling was measured BEFORE any allocator code was written**, as
plans/jit-registers.md requires, via a new env-gated audit
`MYLANG_REGAUDIT=1` in `pick_cached_slots`. Over bench/my + samples +
tests/functional: 127 fragments, 332 qualified int candidates, 220
pinned, **112 with no register**, of which 35 have a live interval
disjoint from some pinned candidate's - and **0** are edge-closed.

So the obvious next increment, letting two slots with disjoint live
ranges SHARE a register, would give a register to **no slot anywhere in
the corpus**. Two reasons, the second general:

  - the `8N_regs_int_*` family reports `shareable=0` outright: every
    accumulator is updated every iteration, so the live ranges all span
    the loop and overlap completely. Sharing cannot help the very
    benches written to exercise register pressure;
  - where intervals ARE disjoint (68_nested: 41 candidates, 4 pinned,
    34 disjoint) none is edge-closed, because a fragment containing
    `if`s has forward branches over nearly every interior point.

Sharing therefore needs real edge reconciliation - moves inserted on
control-flow edges - not the cheap "edge-closed intervals" rule. Large
machine, measured-zero benefit on today's corpus; not built.

**What the audit says instead: the POOL is the binding constraint** (42
candidates against 6 registers on 83_regs_int_40). Which register can
join is decided by how often the emitter hardcodes it as scratch:

    RAX 297   RCX 165   RDX 133   RSI 84   RDI 76   r8 42   r9 14

r9 is used in exactly two local scopes and both are already safe under
the gates r10/r11 rely on - `emit_sync_push_native` (a run containing a
MyLang call gets no caller-saved pin at all) and `emit_ret_native`
(whose first act is `flush_cache()`). As a SysV argument register it can
also hold a 6th helper argument, which is harmless because
`emit_call_prologue` spills every caller-saved pin BEFORE the arg setup.

**MEASURED, and it is the corrected cost model verbatim:**

  - execution-proven: `mov r9, a4` in the emitted entry - a local that
    used to live in memory now has a register;
  - loop instruction count **byte-identical** - the scale3-minus-scale1
    delta is 328,000,022 on both sides of 80_regs_int_08, and the whole
    +0.24% whole-program Ir is a FIXED 496,237-instruction compile-time
    delta, identical at both scales;
  - loop data references **-20.0%** on 80_regs_int_08, -2.7% on
    83_regs_int_40;
  - wall clock **1.009x** over the seven affected benches, i.e. flat
    inside this box's ~5.8% same-binary spread.

A register removes traffic, not work. This is a PREREQUISITE for the
instruction lever A can then delete, exactly as the r10/r11 extension
was - and it is banked as such, not as a win.

### The test the improvement ate

`jit_telide_c3` declared six hot locals to overflow a 4-wide pool and
reach the type-elision tier. With the pool at 7 they all got REGISTERS,
nothing was elided, and it failed with *"no type-elided fragment ever
entered"*. Not a miscompile - a coverage test whose shape the
improvement consumed, the vacuous-test trap running in reverse.

Fixed by DERIVING the program from the pool: `jit_pin_budget()` is
exported and the test generates `budget + 3` accumulators, so widening
the pool can never make it vacuous again. Its expected value could then
no longer be a hardcoded constant, and the oracle is now the program's
SPEC recomputed in C++ (same sequential order, same int_type
wraparound) - not a second engine, since a script's runtime symbol map
is asserted empty and the tree-walker cannot be driven in-process from
a test.

## #96 - r8 joins CONDITIONALLY (pool 8), and the ceiling this arc has hit

**r8 is the cheapest register left, and for a reason that generalises:**
freeing rax/rcx/rdx/rdi needs the emitter to allocate its SCRATCH (~670
hardcoded uses: RAX 297, RCX 165, RDX 133, RDI 76), whereas rsi and r8
hold pinned CONSTANTS - nothing computes into them. And r8's constant,
`t_float`, is only materialised when `run_has_float`, so in an INT-ONLY
run r8 holds nothing at all and can take a pin.

Every `R8R` use in jit.cpp is in `emit_sync_push_native` or
`emit_ret_native` - **the same two functions, safe for the same two
reasons, that admitted r9**: a run containing a MyLang call takes no
caller-saved pin (`jit_run_blocks_xcache`), and `emit_ret_native`
flushes on its first line. r8 sits LAST in `XCACHE_REGS` so passing a
count of 3 instead of 4 excludes it, with no second array.

Pinned by two cases in `jit_xcache_pins`, in both directions - eight hot
int locals reach r8; the same loop WITH a float keeps it as the type
singleton. **Watched failing:** delete the `run_has_float` gate and
`-rt` fails. The VALUE is the oracle there, not the counter: a pinned r8
in a float run is overwritten by the t_float constant.

### ⛔ THE ARC'S RESULT, STATED PLAINLY: MORE REGISTERS DO NOT PAY HERE

Three widenings now, pool 4 -> 8, each measured the same way:

| step | loop D refs | loop instructions | wall clock |
|---|---|---|---|
| r10/r11 (#96) | -23.3% (80_regs_int_08) | unchanged | 1.004x |
| r9 | -20.0% | **byte-identical** | 1.009x |
| r8 | **-40.0%** | **byte-identical** | 0.994x |

**Not one of them changed the instruction count, and none moved the wall
clock.** The reason is structural, and it is now measured three times
rather than argued: in these loops every accumulator is read AND written
every iteration, so a register turns `mov rax, [rbx+0x38]` into
`mov rax, r8` - one instruction either way. Memory traffic falls; work
does not. An instruction is only REMOVED when the value is DEAD
(`skip_write`, which needs liveness and never fires on an accumulator)
or when a reload is redundant (lever A's local forwarding, already
landed - and that one bought **0.89x** on 83_regs_int_40 for -8.86% Ir).

So the corrected cost model's "a register is a PREREQUISITE, not a win"
is not a caveat on this arc - it is the whole result of it. Registers 5
through 8 bought 40% of the memory traffic on the bench built to want
them and zero time.

**What this predicts for registers 9-13** (rax/rcx/rdx/rdi via a scratch
allocator): the same shape, at a far higher cost to build, unless the
extra registers are spent on something that REMOVES instructions.
Before building the scratch allocator, find the instruction that the
extra register makes removable - the way local forwarding did - and
measure THAT.

## #96 steps 1-3 - THE TYPE SINGLETONS LEAVE THE REGISTER FILE (2026-08-17)

**The problem.** `t_int` and `t_float` are `Type *` constants written
into a slot's type field constantly. x86-64's
`mov qword [rbx+disp], imm32` SIGN-EXTENDS its immediate, so it can
store a pointer only if that pointer is `< 0x8000'0000`; the binary is
PIE, so they land around `0x6348'0000'0000` and do not fit. That single
encoding fact is the whole reason they were kept in REGISTERS (rsi/r8),
and being caller-saved, the emitter re-materialised them after every
helper call: **108 `movabs rsi` + 106 `movabs r8` on 09_fib_recursive**,
a program with no float arithmetic in it.

**Step 1 - the low-address arena (`src/lowmem.h`).** One 4 KB
`mmap(MAP_32BIT)` bump allocator; `AllTypes`' 14 singletons are
constructed in it at static init. Not a general allocator and must not
become one - ~14 objects, made once, never freed. ⛔ **"Low absolute
address" is the requirement, not "near the code"**: proximity buys
RIP-relative addressing, which is a two-instruction LOAD and therefore
worse than the register it would replace. The fallback (`new`) is
REACHABLE, not decoration - `MAP_32BIT` can fail, and Darwin has a 4 GB
`__PAGEZERO` - so every site tests its own pointer with
`ml_lowmem_fits_imm32` and picks the encoding per instruction.

**Step 2 - the STORE seam.** `store_type_tag(disp, tag, fallback_reg)`,
one place, imm32 when it fits and the register otherwise. Zero
register-based tag stores corpus-wide afterwards.

**Step 3 - delete what that made dead.** The two materialisation sites
(`emit_type_tags`, `emit_call_epilogue`) emit only on the fallback path,
and `cmp_reg_tag(reg, tag, fallback)` is the READ twin of the store
seam. `jit_xcache_count` (a prefix COUNT) became `jit_xcache_busy` (a
per-register MASK) OR'd into `e.reg_busy` - behaviour identical, but a
count can only retire the LAST pool entry and the remaining pool work
needs to name a register and a reason.

**MEASURED** (bench/my + samples, emitted code):

| metric | before | after | delta |
|---|---|---|---|
| `movabs` instructions | 21,758 | 14,846 | **-31.8%** |
| all emitted instructions | 106,939 | 99,748 | **-6.72%** |
| 09_fib_recursive code | 22,275 B | 21,055 B | -5.5% |
| 54_mandelbrot code | 5,164 B | 4,844 B | -6.2% |

**WALL CLOCK: FLAT.** Suite geomean `cur/base` **1.006x** (interleaved,
`OPT=1 ASSERTS=0`, `-npc`), 18 benches faster / 45 flat / 22 slower.
A `movabs reg, imm64` is 10 bytes but has no operand and no memory
access, so it retires nearly free beside the real work; the win is
CODE SIZE and a freed encoding, not cycles. Same finding as the
guard-elision family - **do not push this line further on Ir evidence
alone**. It is banked as a PREREQUISITE: the tags no longer occupy an
encoding, which is a necessary step toward the register pool.

**⛔ AND IT IS NOT SUFFICIENT. Freeing a register from a CONSTANT does
not free the REGISTER.** rsi is also SysV argument 2 plus ~84 raw
scratch sites (`mov rsi, rax`, `lea rsi, <slot>`), most outside any
`emit_call_prologue` bracket; r8 is argument 5 plus ~20. Adding rsi to
`XCACHE_REGS` fails `-rt` immediately. Every remaining register is
blocked on the emitter ALLOCATING its scratch - the large remaining
piece of #96 and the only route to 13.

**The bug this cost twice** - a wrapper whose NAME encodes its operand
is invisible to a grep for the operand - is recorded as the SIXTH
audit-table shape in CLAUDE.md. The second failure is the one to
remember: the #95 nested-store tier still EMITTED correctly and its
emitted-code counter read 0/64, because what broke was its GUARD.

**A MEASUREMENT TRAP FOUND HERE (worth reusing).** 39_find_builtin read
**1.45x SLOWER**, reproduced at 1.54x on a filtered re-run - and it is
NOT this change. The proof is not statistical: the slowdown is present
under **`-nj`**, where the JIT never emits and none of this diff can
execute. Callgrind agrees - Ir identical with the JIT on (Δ 15,879 of
136.5M) AND off (Δ 654), and cachegrind's D1/LLd/I1 are identical to
three digits. Two LTO links of a 48 MB binary that differ by 640 bytes
laid the interpreter out differently; this is the same real-CPU
front-end effect already recorded for `vm_dispatch`. **When a bench
moves and Ir does not, check whether it still moves with the subsystem
DISABLED before attributing it.**

(Aside found while measuring this, and fixed: `make OPT=1 LTO=0` did not
build - `-Werror=clobbered` on `ok` in `jit_str_probe_verify`. Not a
spurious warning but real UB, and `-Wclobbered` is invisible to an LTO
build, which is why nothing had ever seen it. There is an `lto0` CI lane
now; see the non-LTO note in CLAUDE.md.)

## #96 - r9 was NEVER safe as a pin (a shipping wrong answer, 2026-08-17)

`939f5a9` put r9 in the caller-saved pin pool. It was a **wrong answer
in the default shipping configuration for one day**, and it is the most
instructive failure of this arc, so the causes are worth more than the
fix.

**The fix is one line**: `XCACHE_ORDER` is `{ 10, 11, 8 }`. r8 stays -
re-audited properly this time, its raw-scratch use really is confined to
`emit_sync_push_native` / `emit_sync_call_inline` (blocked by
`jit_run_blocks_xcache`) and `emit_ret_native` (whose first act is
`flush_cache()`).

**The repro**, twelve lines, no lever, no flag:

```
var cap = 3;
var f = func[cap](int n) {
    var s0 = 0; var s1 = 0; var s2 = 0; var s3 = 0;
    var s4 = 0; var s5 = 0; var s6 = 0; var s7 = 0;
    for (var i = 0; i < n; i++) {
        s0 += i; s1 += i * 2; s2 += i * 3; s3 += i * 4;
        s4 += i * 5; s5 += i * 6; s6 += i * 7;
        s7 += cap;
    }
    return s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7;
};
print(f(64));
```

`-tw` and `-nj` print **56640**. The JIT printed **88854283473440**
(OPT=0) and **97861752749472** (OPT=1 ASSERTS=0). The entry emits
`mov r9, s5`; `emit_ctx_chain_r9` then walks the ctx through r9.

The claim that admitted it - "r9 is used in exactly TWO local scopes and
both are already safe by the SAME gates r10/r11 rely on" - was false. r9
is raw scratch in the capture ops and in **every element tier**
(`emit_elem_int_read`, `emit_elem_bounds_or_wrap`, `emit_elem_base_gate`,
`emit_store_elem_inline`, `emit_load_elem2_inline`,
`emit_store_elem2_inline`, `ForStepElemInt`), ~80 sites.

### Why nothing caught it, and the three nets that now do

**1. The census was blind to its own subject.** `scripts/regcensus.py`
was written two commits earlier *specifically* to count this, and
reported **14** sites for r9. The truth is **87**. The misses are
`movabs_r9`, `cmp_r9_rdx`, `lea_rdi`, `slots_to_arg0`,
`store_elem_byte_dil` - the register is in the **method name**, so a
scan for the operand cannot see it. That is the SIXTH audit-table shape
already documented in CLAUDE.md, walked into by the tool built to avoid
it. It now DERIVES the accessor set from the source (every `void name(`
whose name, split on `_`, holds a register token), so a new fixed-pair
wrapper is counted the day it is written. Corrected table:

| reg | unbracketed | was |
|-----|-------------|-----|
| RAX | 392 | 254 |
| RCX | 193 | 135 |
| RDX | 142 |  76 |
| R9  |  87 |  14 |
| R8  |  45 |  42 |
| RDI |  33 |  14 |
| RSI |  23 |  23 |

**2. A pool ordered by preference hides its own tail.** `take_reg` scans
in preference order, so r9 - 4th of 4 - was handed out only to a run's
SEVENTH pin. `-rt`, all four differentials, `corpus_diff` and every
fuzzer hammered the first three and reached r9 essentially never. A
bigger corpus does not fix that; the allocator's own preference is the
hole. **`MYLANG_JIT_XROT=N` rotates the pool** so member N is first;
`corpus_diff.sh --xrot` runs the matrix, and `jit_xcache_pins` sweeps
every rotation in-process. Making r9 first fails `-rt` in seconds -
which is exactly how this was found.

**3. "Safe by the same gates as X" is not an argument.** A register
joins only with its own sites enumerated against the gates. That is now
mechanical: **`Emitter::scratch(reg)`**, called at the top of each
raw-scratch emitter, ML_CHECKs that no pin lives there and aborts naming
the emitter. Ten sites. It is not called inside an
`emit_call_prologue`/`epilogue` bracket, where a pin is legitimately
spilled - which is precisely the (a)/(b)/(c) split the census draws.

### And the oracle for all of this had quietly stopped working

`scripts/vdjcmp.sh` - "the oracle for a pure restructuring" - reported
**0 identical / 108 differing** for any two separately-linked binaries,
and **77/31 for a binary against ITSELF**. Two independent holes, both
opened by earlier work in this same arc:

- **#96 step 3** moved the Type singletons into a low-address arena so a
  tag encodes as an `imm32` - and the disassembler prints an imm32 in
  **decimal** (`mov r2.type, 1095139376`). Every masking rule was
  hex-only.
- an address the disassembler **mis-decodes** emerges as individual
  `.byte 0xe0` / `nop` lines, with no maskable token at all. No regex
  reaches that; the script now runs both binaries under **`setarch -R`**,
  which removes the nondeterminism instead of hiding it.

It **self-tests** now (the same binary twice must be 100% identical,
else exit 2 before reporting anything). Without that it cannot tell
"your change altered the code" from "the normalisation stopped covering
something" - and it reported the second as the first for weeks.
**A normaliser fails in the "everything differs" direction, which reads
exactly like a catastrophic change**; a 0-identical result is a reason
to read one diff, never a reason to believe the change broke everything.

### What this means for the rest of #96

The remaining pool candidates are **not** a matter of counting sites and
picking the smallest. RDI's 33 unbracketed sites are (a) SysV setup
inside a call bracket - already safe; (b) SysV setup in hand-rolled call
sequences, already blocked because the run contains a call; and (c)
genuine scratch, which for RDI is exactly **three opcodes**
(`StoreElemInt`, `StoreElemFloat`, `StoreElem2V`, via
`emit_store_elem_inline` / `emit_store_elem2_inline`). Only (c) needs
work, and the shape of that work is a **per-opcode clobber mask**
replacing the single all-or-nothing `jit_run_blocks_xcache` gate.

But note what the r8 entry above already measured: **more registers do
not pay here**. Any further pool widening must be justified by a
measurement, not by the site count being small - and, given that
finding, the honest next step for this task is the *scratch allocator*
(which is what unlocks RAX/RCX/RDX, the only registers numerous enough
to matter), not another opportunistic pool addition.

## The native disassembler was WRONG, and its callers hid it (2026-08-17)

`-vdj` is the evidence for every claim in this file - which tier fired,
whether a refactor changed anything, why a guard is where it is. It was
measured against the corpus for the first time on 2026-08-17 and could
not decode **5543 bytes**. Nothing said so; the dump simply printed
confident, well-formed, wrong mnemonics.

**Six opcodes were missing from `decode_one`**, all emitted by jit.cpp
today: `03`/`2B` (add/sub r64, r/m64 - only the r/m <- reg direction was
decoded, so the M5b record-push address arithmetic decoded as nothing),
`3D` (cmp rax, imm32, the accumulator short form that #96 step 3 made
common - 168 sites), `63` (movsxd), `6B` (imul r64, r/m64, imm8 - the
imm32 twin was there, but 0x30 is the stride the assembler picks), and
`88`/`8A` (the flat `array<bool>` byte store).

**And the SIB arm was wrong in three ways**, the first of which
desynchronised the whole fragment: it returned WITHOUT consuming the
displacement, although a SIB byte does not replace disp8/disp32 - mod
still selects one. So `mov rdi, [rsp+8]` printed as
`mov rdi, [rsp+rsp*8]`, left `08` behind as a stray `.byte`, and every
instruction after it was decoded at the wrong offset. It also hardcoded
scale `*8` and printed `rsp*8` for index==4, which MEANS NO INDEX.

Fixed: **5543 -> 0** undecoded bytes, corpus-wide.

### The determinism half, and why masking was the wrong answer

Baked addresses move with every process under ASLR, so `-vdj` was not
reproducible - a binary's dump differed from ITSELF. They now print as
`<int-tag>` / `<float-tag>` / `<array-tag>` / `<addr>` / `<helper>`:
the operand SHAPE, which is all a reader or a differ needs, with the
digits behind `MYLANG_VDJ_ADDRS=1`.

The previous answer was a sed pipeline plus `setarch -R` inside
`scripts/vdjcmp.sh`, and it failed exactly the way workarounds do:

 - **it rotted silently.** The masks were hex-only; #96 step 3 made
   Type tags `imm32`, which the disassembler printed in DECIMAL. From
   that commit the script reported **0 identical / 108 differing** for
   any pair of binaries. It was not an oracle, it was a constant
   "everything changed" - and nobody noticed, because that is also what
   a genuinely broken change looks like;
 - **no regex could have sufficed.** A mis-decoded instruction emerges
   as bare `.byte 0xe0` / `nop` lines with no maskable token, which is
   why 31 of 108 programs differed from themselves;
 - **it left the tool broken for its most important consumer** - a
   human reading the dump.

`vdjcmp.sh` is now a plain `cmp` with a self-test that refuses to report
anything if one binary gives two different dumps.

### The self-check, and three vacuous tests before it worked

The fragment walker counts undecoded bytes AND **skipped op marks** - a
mark is an offset the JIT recorded at a real instruction boundary, so
stepping past one proves the decode drifted - and prints
`⛔ DUMP IS UNRELIABLE: N undecoded byte(s), M skipped op mark(s)`.

The `-rt` check `jit: -vdj decodes every emitted form, reproducibly`
dumps 14 programs spanning the emitter's shape space and requires no
`.byte`, no skipped mark, and two identical dumps. **It was vacuous
three times before it caught anything**, and each failure is a distinct
instance of the trap:

 1. its vacuity guard counted `enter.nat` - a BYTECODE op name the
    listing prints whether or not machine code was emitted;
 2. **`-vdj` is `g_jit_annotate`, not a dump flag.** Without setting it
    the dump has no native section at all, so the test was inspecting
    text that could not contain the bug. The corrected guard (count
    `---- native x86-64` blocks) surfaced this as "0 native blocks over
    14 programs";
 3. its first 11 programs were all statically typed, and `cmp rax,
    imm32` lives in a boxed TYPE CHECK - a float conversion chain and a
    string concat were required. The test had been written from the
    shapes the author had in mind, not the shapes the emitter produces.

Watched failing: dropping `0x3D` from `decode_one` fails `-rt` naming
three programs.

**The rule this earns, and it generalises past the JIT: fix the TOOL,
not the script that reads it.** A workaround in one consumer leaves the
tool broken for every other consumer, and then rots the next time the
tool's output changes shape.

## 2026-08-18 - the NO-ARENA configuration, and the wrong answer it hid

`MYLANG_NO_LOWMEM=1` refuses the low-address arena, so the JIT's
register-form type tags become reachable by a test.

**WHY IT HAD TO EXIST.** `lowmem.h` places `t_int`/`t_float`/`t_bool`
below 2^31 so a tag store is `mov qword [rbx+d], imm32`. Where that
mapping is unavailable - Darwin, Windows, a hardened kernel, an
exhausted low 2GB, **a failed `MAP_32BIT` on an ordinary Linux box** -
`store_type_tag` and `cmp_reg_tag` fall back to a REGISTER form. The
header says in bold that this is reachable on Linux. It is also
materially different emitted code, and it decides whether rsi/r8 can
hold a pin at all (`jit_xcache_busy`). Nothing could enter it: the
arena is an `mmap` at static init with no switch. Same shape as the
`lto0` lane - a configuration only one platform can take is one nobody
tests.

**WHAT ITS FIRST RUN FOUND - a shipped wrong answer.** `store_dst_bool`
wrote the bool tag as

    e.store_type_tag(a.type, t_bool, RCX);        /* RCX holds it */

and #96 step 3, making tags immediates, DELETED the `movabs RCX,
t_bool` that made the third argument true - correctly noting it as "an
instruction removed rather than relocated", which it is on the arena
path. Nothing else ever loads a tag into RCX. So on the fallback path
every bool store wrote **whatever RCX happened to hold as the slot's
type pointer**. Measured: 7 of 107 corpus programs crashed or answered
wrongly (`57_bool_reduce` -> a null `Type *` in the ret audit;
`34_sort_custom_cmp`, `35_map_filter`, `79_dyn_float`,
`06_calls_closures`, `dyn_template_base`).

WATCHED, with the defect reintroduced and the new ML_CHECK removed:

    ./mylang -rt                     1922/1922  PASS   (blind)
    corpus_diff.sh                   20/20 agree       (blind)
    corpus_diff.sh --nolowmem        18/20             CATCHES IT

**THE FIX IS A SEAM SPLIT, not a re-added instruction.**
`store_type_tag(disp, tag, held_reg)` now ML_CHECKs that `tag` is one
of the two singletons something actually materialises (t_int in rsi at
every fragment entry, t_float in r8), because that third parameter is a
promise a caller can break SILENTLY - and one did. Any other tag must
use `store_type_tag_via(disp, tag, scratch)`, which BUILDS the value
when it is not an immediate. `cmp_reg_tag` carries the same tripwire.

**THE RULE: an optimization that makes a register UNNECESSARY must not
leave behind an argument that says the register is still LOADED.**
Delete the parameter or honour it.

Nets: `tests/corpus_diff.sh --nolowmem`, plus `-rt` under the same env,
both in the `Nets` CI lane's `differential` job.

## 2026-08-18 - #96: the xcache gate becomes a per-register CLOBBER MASK

`jit_xcache_clobber(chunk, begin, end, has_hoist)` replaces

    xcache_ok = hregs.empty() && !jit_run_blocks_xcache(..) && !lever

with one mask, one bit per pool register, and each contributor stating
its OWN claim: the C1 hoist owns r10/r11 (`g_hoist.rdata`/`rcount` and
nothing else - the region preheader's other scratch is rax/r9/rdx,
which are in no pool); a MyLang call emitter uses r8/r10/r11 as raw
scratch outside any prologue bracket, so it claims the whole pool; a
type singleton still in a register claims that register; the kill
switch claims everything. `e.reg_busy` is seeded from it, so the budget
and the assignment cannot disagree.

**WHY THE BOOLEAN WAS EXPENSIVE.** It denied the whole pool for one
hoist region, though a region claims two of three members - and the two
conditions are not independent: an element read on a loop-invariant
base is PRECISELY what makes `jit_hoist_pick` return a region. So
"walks an array in a loop" and "may pin a caller-saved register" were
mutually exclusive by construction. Measured over bench/my + samples +
tests/functional: **199 runs compiled, 20 with a hoist region, 0 of
those with a pool register left.** That is what made the previous day's
ScratchPlan unreachable.

**A SECOND GATE OF THE SAME SHAPE, one register over.** With the mask
in, the reachability was STILL 0/20 - and the breakdown said none of
the 20 was blocked by a call; all 20 were blocked by
`jit_xcache_busy`'s float-tag claim. That predicate is
`run_needs_float_tag`, a deliberately fail-safe whitelist of the pure
int-loop family, so EVERY element op falls to `default: return true`.
It was claiming r8 for a singleton that, with the low-address arena, is
not in a register at all. Narrowed to
`run_needs_float_tag(..) && !jit_tag_is_imm(t_float)` - which is
exactly the form `elem_reg_usable` already used, so the two agree now.
After both: **20 of 20.**

**AND IT MEASURES FLAT.** 9 of 108 corpus programs change
(43_sieve, 56_sieve_bool, 60_bit_sieve, 68_nested, 80..83_regs_int,
samples/primes2). Callgrind Ir, `OPT=1 ASSERTS=0` both sides:
43_sieve **-0.40%**, 56_sieve_bool **-0.71%**, 60_bit_sieve -0.15%,
68_nested +0.22%, the four regs_int benches 0.00%. Wall clock,
interleaved `--baseline`, full suite: **geomean cur/base 0.997x**, per
bench 0.92x .. 1.04x, i.e. inside the spread.

**THE THIRD "MORE REGISTERS DO NOT PAY HERE" IN THIS ARC** (after r8
and after the ScratchPlan). The arc's central result stands: pin
PRESSURE is not what these programs are short of. What the mask buys is
structural - the gate now states a fact instead of an approximation,
which is what makes any future pool member reachable at all - and the
narrowing removes a claim that was simply FALSE on the shipping path.

Also fixed here, because the wider budget exposes it: the C2b hoist
pair picked its registers as `CACHE_REGS[hot.size()]` under
`MAX_CACHED - hot.size() >= 2`. That subtraction is a `size_t` and was
safe only while a hoist run got no caller-saved pin - the exact
coupling this change removes. With 5 pins it underflows, grants the
pair unconditionally and indexes `CACHE_REGS[5]`. The displacement arm
needed the same care in the other direction: dropping the pin that
lives in r8 frees no CALLEE-saved register, so "the two coldest" became
"the ones that do not fit in what is left", weighed against exactly
those.

PINNED by a new `jit_xcache_pins` case, "a C1 hoist region leaves r8
pinnable (r10/r11 are its own)" - five accumulators plus an `a[i]` read
to force a region. Watched: restoring the boolean fails it by name at
every `--xrot` rotation. `-rt` 1922/1922 arena on and off, all four
corpus_diff matrices 20/20.

## 2026-08-18 - WHY A PIN IS WORTH ZERO: the operand routing, not the pool

`MYLANG_JIT_MAXPINS=N` caps the pin budget so the marginal value of one
register is measurable. On 83_regs_int_40 (40 independent int
recurrences) EVERY pin from 0 to 7 measures +0.00% Ir. The emitted code
says why, and it is one line of diff:

    cap=0  (a0, i in memory)      cap=7  (a0->r14, i->r13)
    mov rax, a0                   mov rax, r14
    mov rcx, i                    mov rcx, r13
    add rax, rcx                  add rax, rcx
    mov a0, rax                   mov r14, rax
    sar rax, 3                    sar rax, 3
    mov rcx, a0                   mov rcx, r14
    xor rax, rcx                  xor rax, rcx
    mov a0, rax                   mov r14, rax

**A pin changes each operand's ADDRESSING MODE, never the instruction
COUNT.** The loop body is 340 instructions per iteration at both caps;
209 of them are `mov` and only 122 are work (42 add, 40 xor, 40 sar).

THE CAUSE is that the B1/B2 specialized arithmetic emitter is a fixed
three-address-through-the-accumulator shape: operand 1 into RAX,
operand 2 into RCX, apply, store RAX to the destination. A pinned
register can be the SOURCE of a `mov`; it can never BE the operand of
the `add`. So the pin removes a load's latency (real, but invisible to
Ir and small against 40-way ILP) and removes no instruction.

**THE FIX IS TWO-ADDRESS ARITHMETIC, and it mostly does not need a pin
at all.** Every accumulator has dst == src1 (`a = a + i`, and every
`+=` in the language):

    pinned dst    add r14, r13                       4 -> 1
    memory dst    mov rax, i ; add [rbx+d], rax      4 -> 2

`add r/m64, r64` is a legal encoding, so the memory form pays on the 33
accumulators that will never get a register. It composes with C3's tag
elision (the form writes the payload only). And it is what finally
gives an extra register something to do: with two-address arithmetic a
pinned accumulator is 1 instruction where memory is 2 - the first
REASON to widen the pool this arc has produced, as opposed to a site
count.

Secondary, same family: the loop counter emits
`mov rax,r13; movabs rcx,1; add rax,rcx; mov r13,rax` where `add r13,1`
is one instruction - `movabs` for a value that fits imm8, the same
waste the type-tag arena removed one level up.

**THE LESSON FOR THE ARC: three increments widened the pin pool and all
three measured flat, because the pool was never the constraint. When an
optimization measures zero, read the EMITTED CODE for the shape it was
supposed to improve before concluding anything about its size.**

## 2026-08-18 - #96 increment 1: TWO-ADDRESS arithmetic, memory destination

`<op> qword [rbx+d], reg` for `dst = dst OP b` - every `+=` in the
language and every accumulator recurrence. Replaces the emitter's
load/load/apply/store shape:

    mov rax, a0 ; mov rcx, i ; add rax, rcx ; mov a0, rax     4
    mov rax, i  ; add [rbx+d], rax                            2
    add [rbx+d], rax          (b already forwarded in RAX)    1

`op_mem_reg` is the encoder (MR form). No pin is involved, so it pays
on accumulators that will never get a register.

**NO TAG STORE**, and it is not an omission: dst is READ as an int by
this very op, so its tag is already `t_int`. Same argument C3's elision
makes, available here without C3 having to prove anything.

FIVE conditions, three SOUNDNESS and two COST, each sabotage-tested and
labelled in the source by what the suite actually answered:
 - dst == operand a, a is a slot (the shape itself);
 - dst NOT pinned - writing memory while the live value sits in a
   pin register is a wrong answer. WATCHED: removing it HANGS `-rt`;
 - `imul` excluded - no MR encoding exists. WATCHED: removing it aborts
   in op_mem_reg BY NAME;
 - dst not forwarded OUT - **COST, not soundness**. Using the form
   there is correct (g_fwd is cleared, so the consumer reloads from the
   memory just written); it only forfeits lever A's elision. WATCHED:
   removing it leaves `-rt` fully green, which is why it is labelled
   COST and must not be "tidied" away on a green run;
 - `fa` and a ref-listed dst - COST, declined pending a measurement.

MEASURED (callgrind Ir, OPT=1 ASSERTS=0): **83_regs_int_40 -17.05%,
82_regs_int_25 -15.06%, 81_regs_int_14 -11.30%, 80_regs_int_08
-5.92%**, 54_mandelbrot -0.13%, every other bench byte-flat. 10 of 108
corpus programs change. `func work` in 83: 817 -> 747 instructions.

**⛔ AND THE WALL CLOCK IS FLAT - geomean cur/base 1.005x.** The reason
is specific and generalises: an x86 read-modify-write decodes to the
SAME micro-ops as the sequence it replaces -

    mov rcx,[a] ; xor rax,rcx ; mov [a],rax     3 uops
    xor [a], rax                                3 uops

a load, an ALU op and a store either way. **Callgrind counts
INSTRUCTIONS; fusing load/op/store into an RMW changes that count
without changing the work.** This is a DIFFERENT mechanism from the
guard-elision divergence already recorded here (a predicted branch over
L1 loads retiring free) and deserves its own line: when an optimization
merges memory access INTO an arithmetic instruction, expect Ir to
overstate it.

What is genuinely bought is decode slots and code size - and the
precondition for increment 2. The REGISTER form (`add r14, r13`, a
pinned dst) removes real work: ONE uop against four instructions of
which two merely rename. Read this entry as "the MEMORY half pays in
size, the REGISTER half should pay in time", not as "two-address
arithmetic does not pay".

Reach counter `two_addr` in MYLANG_JITSTATS, bumped from EMITTED code
through RCX (not `bump_op`, which hardcodes rax - rax may hold the
forwarded operand) and emitted BEFORE the arithmetic, since the bump
clobbers FLAGS. Pinned by the `jit: two-address arithmetic` `-rt` case:
four programs, each asserting the VALUE and whether the tier engaged.
Two of its cases needed TEN accumulators rather than one - with one,
every accumulator is PINNED and the tier declines for that reason
first, so the imul sabotage left the suite green. A decline case that
cannot fail is not a decline case.

## 2026-08-18 - #96 increment 2: TWO-ADDRESS with a PINNED destination

`<op> pin, <src>` - the half that removes real WORK rather than
re-encoding it:

    mov rax, r14 ; mov rcx, r13 ; add rax, rcx ; mov r14, rax    4
    add r14, r13                                                 1

Four instructions - of which the two moves are eliminated at rename but
still occupy decode slots - become ONE uop. Contrast increment 1, whose
memory RMW decodes to the same uops as the sequence it replaced.

Three source shapes, no scratch needed by any of them: another pin
(`add r14, r13`), a memory slot (`add r14, [rbx+d]` - the RM form reads
memory directly), or a literal (`add r14, 1`). Encoders `op_rr2` /
`op_reg_slot` / `op_reg_imm` share one opcode table, because RM is
`reg = reg OP r/m` and reg-reg and reg-memory differ only in the ModRM.

**`imul` IS ALLOWED HERE**, unlike the memory half: `imul r64, r/m64`
(0F AF /r) is exactly the RM form and only the MR direction is missing
from the ISA. That asymmetry is why the two halves have different op
sets, and it is worth stating because it looks like an inconsistency.

**A forwarded-OUT destination is ALSO allowed here**, again unlike the
memory half: the result is in a PIN, so handing it to the consumer
costs one `mov rax, pin`, and 1 + 1 still beats 4. Declining it left
`a0 = a0 + i` at four instructions on 83_regs_int_40 - the single
biggest remaining cost when the pinned form first landed.

The whole recurrence, before and after this arc:

    mov rax, a0 ; mov rcx, i  ; add rax, rcx ; mov a0, rax
    sar rax, 3  ; mov rcx, a0 ; xor rax, rcx ; mov a0, rax     8
    ---------------------------------------------------------------
    add r14, r13 ; mov rax, r14 ; sar rax, 3 ; xor r14, rax    4

**MEASURED, increments 1+2 together (callgrind Ir, OPT=1 ASSERTS=0):**
01_while_loop **-37.35%**, 80_regs_int_08 -27.13%, 81_regs_int_14
-24.79%, 82_regs_int_25 -23.16%, 83_regs_int_40 -22.29%,
07_nested_loops -11.34%, 43_sieve -6.88%, 54_mandelbrot -3.43%.
`func work` in 83: 817 -> 722 instructions. 49 of 108 corpus programs
change.

**WALL CLOCK: geomean cur/base 0.990x** - a suite-wide 1% gain, with
01_while_loop at **0.62x** and 81_regs_int_14 at 0.86x. This is the
first wall-clock win of the #96 arc, and it lands exactly where the
uop argument said it would.

Also here: `op_reg_imm` prefers the **imm8** form (`83 /ext`, 4 bytes)
over imm32 (`81 /ext`, 7). Deliberately NOT `inc`/`dec`, which are one
byte smaller again but write flags PARTIALLY (they leave CF), costing a
flag-merge on older cores - a real trade for one byte where imm8 is
free and covers every small constant, not just +-1.

⛔ TWO ENCODING TRAPS, both watched failing:
 - **`imul r64, r/m64, imm32` (69 /r) names the destination TWICE** -
   reg field AND r/m - so a high register needs REX.R *and* REX.B.
   Setting only REX.B (natural, since every other form here is
   r/m-only) leaves the reg field at its low 3 bits: for r12 that is 4,
   i.e. **RSP**, and `imul rsp, r12, 3` destroys the stack pointer.
   ASan reported a stack-overflow with sp = 0x3. The case that caught
   it was the imul DECLINE program written for increment 1, which
   reaches this path the moment a pinned dst is allowed.
 - **the opcode lookup must not live INSIDE its ML_CHECK.** Written as
   `ML_CHECK_MSG(op_rm_opcode(aop, opc, two, ext), ...)` it compiles
   away entirely under ASSERTS=0, leaving `opc`/`two` UNINITIALISED and
   a release build emitting a garbage opcode. `-Werror=uninitialized`
   caught it; the debug build was clean. Same family as any check whose
   side effect is load-bearing.

## 2026-08-18 - the MAXPINS sweep RE-RUN: a register is worth something now

The same `MYLANG_JIT_MAXPINS` sweep that found every pin worth +0.00%,
re-run after increments 1 and 2. Callgrind Ir, OPT=1 ASSERTS=0, and the
instrument's self-test (a non-binding cap is a no-op) re-verified
byte-identical on 108/108 first.

**WHAT THE WHOLE POOL IS WORTH (cap 0 -> 7):**

    bench              before      now
    83_regs_int_40     +0.00%     -3.19%
    80_regs_int_08     +0.00%    -12.44%
    01_while_loop      -5.85%    -16.57%
    07_nested_loops    -5.36%    -11.33%

**AND THE MARGINAL VALUE, which is the number that decides the arc:**

    83_regs_int_40   pins 3,4,5,6,7 -> -0.76 -0.61 -0.62 -0.62 -0.62 %
    80_regs_int_08   pins 3,4,5,6,7 -> -2.96 -2.44 -2.50 -2.57 -2.64 %
    01_while_loop    pins 1,2       -> -8.28 -9.04 %, then flat
    07_nested_loops  pins 1,2       -> -8.50 -3.09 %, then flat

⛔ **ON THE REGISTER-PRESSURE BENCHES THE CURVE HAS NOT PLATEAUED AT 7**
- and 7 is the whole pool (MAX_CACHED 4 + MAX_XCACHED 3). Each further
register returns a steady -0.62% on 83 and -2.6% on 80, with no sign of
saturation. The two loop benches DO plateau, correctly: they have two
hot locals, so pins 3+ have nothing left to hold.

**THIS IS THE EVIDENCE THE 13-REGISTER GOAL NEVER HAD.** Three
increments widened the pool against a site count and measured flat;
the honest reading was "pin pressure is not the constraint". It was
not the constraint *while the emitter could not name a pinned register
as an arithmetic operand* - a pin then only changed an addressing mode.
Now that `add r14, r13` exists, a pinned accumulator is 1 uop where a
memory one is 3, and the pool is measurably too SMALL rather than
irrelevant. Widening it is now a decision with a number behind it.

The per-pin figure also says how much: 83 has 40 accumulators and each
pin covers 1/40 of the body (-0.62%), 80 has 8 and each covers 1/8
(-2.6%) - so the return scales with what fraction of the working set a
register captures, exactly as it should, and going 7 -> 13 on 83
projects to roughly another -3.7%.

## 2026-08-18 - #96 increment 3: the COUNTED-LOOP step, in registers

`ForLoopStep` is what a `for` loop lowers its step to, so increments 1
and 2 - which rewrite the specialized IntAddRR/RI family - never
reached it. It was paying the full accumulator shape plus a
materialised constant:

    mov rax, r13 ; movabs rcx, 1 ; add rax, rcx
    mov r13, rax ; mov rcx, n    ; cmp rax, rcx ; jl          7
    inc r13      ; cmp r13, n    ; jl                         3

With the counter PINNED the step is two-address and the bound test
reads the pin directly, so RAX is never involved. New encoders
`cmp_rr2` / `cmp_reg_slot` / `cmp_reg_imm` (the CMP counterparts of the
RM family) let the bound be an immediate, another pin, or memory.
A MEMORY counter keeps the old shape but still drops the `movabs`: a
literal step becomes an imm8/imm32 operand.

**`inc`/`dec` ARE used here** (maintainer-suggested), and the reason it
is safe is specific rather than general: their PARTIAL flags write (CF
untouched) is harmless because the FLAGS ARE DEAD - the `cmp` on the
very next line sets them afresh before the jump reads them. It is also
not a new idiom: IntAddStep's counter already emitted `inc rax`.
Elsewhere the imm8 form (`83 /ext`) is preferred instead, which is one
byte larger but writes flags completely and covers every constant.

**MEASURED, increments 1+2+3 cumulative (callgrind Ir, OPT=1
ASSERTS=0):** 01_while_loop **-37.35%**, 80_regs_int_08 **-31.07%**,
83_regs_int_40 -23.26%, 43_sieve -12.08%, 07_nested_loops -11.35%,
44_primes_sqrt -6.13%, 03_int_arith -5.50%, 46_matrix_mult -0.07%.

**WALL CLOCK: geomean cur/base 0.985x** (0.990x after increments 1+2,
so increment 3 adds ~0.5% suite-wide). 01_while_loop 0.64x,
81_regs_int_14 0.80x, 80_regs_int_08 0.91x, 82 0.92x, 43_sieve 0.94x.

Reach: `step_imm` in MYLANG_JITSTATS. Note it fires only where the
COUNTER wins a pin - a ten-accumulator program spends every register on
accumulators and leaves the counter in memory, which two `-rt` cases
assert as a DECLINE.

⛔ IntAddStep is NOT converted yet - same shape, same opportunity (its
accumulate is still `read/load/op/write` and its bound test still goes
through RAX), left for a follow-up so this increment measures alone.

## 2026-08-18 - #96 increment 3b: IntAddStep, the fused accumulate+step

The sibling of ForLoopStep, and the point worth recording is that its
TWO HALVES ARE INDEPENDENT: the accumulator and the loop counter are
different slots, so either may be pinned without the other. Each half
converts on its own merits and the boxed code still runs for whichever
half is memory-resident - which is also why the conversion could not be
one `if`.

    mov rax,r12 ; mov rcx,rhs ; add rax,rcx ; mov r12,rax
    mov rax,r13 ; inc rax     ; mov r13,rax
    mov rcx,n   ; cmp rax,rcx ; jl                            10
    add r12, rhs ; inc r13 ; cmp r13, n ; jl                   4

The accumulate takes any of the three RM source shapes (pin, memory
slot, imm8/imm32, or a value forwarded in RAX by lever A - `add r12,
rax`, which is the shape `sum += a[i]` produces). The counter half is
ForLoopStep's, verbatim.

`g_fwd` is CLEARED when either half converts: both leave RAX holding
something other than what the boxed shape left there, so no consumer
may believe a forward survived the op.

**MEASURED, cumulative over increments 1+2+3+3b (callgrind Ir, OPT=1
ASSERTS=0):** 01_while_loop **-37.35%**, 80_regs_int_08 -31.07%,
83_regs_int_40 -23.26%, **14_array_subscript -12.69%**, 43_sieve
-12.08%, 07_nested_loops -11.35%, 44_primes_sqrt -6.13%,
**18_foreach_array -5.72%**. The last two named are NEW - they are the
`sum += a[i]` shape, which only IntAddStep reaches.

**WALL CLOCK: geomean cur/base 0.978x**, from 0.985x before this half -
so IntAddStep alone is worth ~0.7% suite-wide. 01_while_loop 0.62x,
81_regs_int_14 0.85x, 82 0.89x, 80 0.90x, 18_foreach_array 0.96x.

NOTE 14_array_subscript reads 1.04x wall for -12.69% Ir: it is
memory-bound, so removing instructions does not move it. The
instruction-vs-time divergence again, in its ordinary form.

## 2026-08-18 - the MAXPINS sweep, third run: the pool is the constraint

After the whole operand-routing arc (increments 1, 2, 3, 3b). Self-test
re-verified first: a non-binding cap is byte-identical on 108/108.

**WHAT THE WHOLE POOL IS WORTH (cap 0 -> 7), across the arc:**

    bench                 pre-arc   after 1+2      now
    83_regs_int_40         +0.00%      -3.19%   -4.11%
    80_regs_int_08         +0.00%     -12.44%  -16.19%
    01_while_loop          -5.85%     -16.57%  -16.57%
    07_nested_loops        -5.36%     -11.33%  -11.34%
    14_array_subscript          -           -  -24.93%

**MARGINAL, per additional pin, and this is the number that sizes the
remaining work:**

    83_regs_int_40   2..7  -0.91 -0.77 -0.62 -0.62 -0.63 -0.63 %
    80_regs_int_08   2..7  -3.60 -3.11 -2.57 -2.64 -2.71 -2.78 %
    01_while_loop    1,2   -8.28 -9.04 %, then flat (2 hot locals)
    07_nested_loops  1,2   -8.50 -3.09 %, then flat
    14_array_subscript 2,3 -12.47 -14.25 %, then flat (3 hot locals)

⛔ **THE PRESSURE BENCHES STILL DO NOT PLATEAU AT 7, AND 80's MARGINAL
IS RISING** - -2.57, -2.64, -2.71, -2.78 for pins 4,5,6,7. Every
register handed out is worth MORE than the one before it, because each
additional pinned slot removes a whole 4-instruction memory shape now
instead of merely changing an addressing mode. 7 is the entire pool
(MAX_CACHED 4 + MAX_XCACHED 3).

**PROJECTION for the pool work.** 80_regs_int_08 has 8 accumulators
plus a counter, so 7 pins leave 2 slots in memory: 7 -> 13 captures
both, ~-5.6%. 83_regs_int_40 has 41 hot slots and each pin is worth
-0.63%: 7 -> 13 projects ~-3.8%. Those are the numbers that justify
the RAX/RCX/RDX scratch allocator (717 of 1016 unbracketed sites),
which is the whole remaining cost of getting to 13.

**AND THE SHAPE OF THE PLATEAUS IS THE CROSS-CHECK.** 01_while_loop
flattens after 2 pins, 07_nested_loops after 2, 14_array_subscript
after 3 - each exactly at its number of hot locals. A sweep that
plateaued everywhere would be measuring something else; a sweep that
plateaued nowhere would be suspicious. This one plateaus precisely
where the program runs out of values to hold, which is what a correct
marginal-value curve looks like.

## 2026-08-18 - the MAXPINS sweep, FOURTH run: pin 8 (rdi) pays the
## projection, and 80's marginal is STILL rising

After admitting rdi (#96 step 2). Callgrind Ir, `OPT=1 ASSERTS=0`.
Self-test re-verified at the NEW budget first - `MYLANG_JIT_MAXPINS=8`
is byte-identical to unset on 109/109 - which is what makes the cap a
measuring instrument rather than a second code path.

**REPRODUCIBILITY CHECK FIRST, because it is the reason to believe the
new column.** The cap 0 -> 7 figures reproduce the third run EXACTLY:

    bench                3rd run   4th run
    83_regs_int_40        -4.11%    -4.11%
    80_regs_int_08       -16.19%   -16.19%
    01_while_loop        -16.57%   -16.57%
    07_nested_loops      -11.34%   -11.34%
    14_array_subscript   -24.93%   -24.94%

So admitting rdi is purely ADDITIVE - it did not perturb what the first
seven pins do - and any difference in the cap-8 column is pin 8 alone.

**WHAT THE POOL IS WORTH NOW (cap 0 -> 8), and PIN 8 alone:**

    bench                cap0->7   cap0->8   pin 8
    83_regs_int_40        -4.11%    -4.72%   -0.63%
    80_regs_int_08       -16.19%   -18.59%   -2.86%
    01_while_loop        -16.57%   -16.57%    0.00%
    07_nested_loops      -11.34%   -11.34%    0.00%
    14_array_subscript   -24.94%   -24.94%    0.00%

**PIN 8 LANDED ON ITS PROJECTION, TO THE DECIMAL.** The third run
predicted -0.63%/pin on 83 and "-2.78% and rising" on 80. Measured:
-0.63% and -2.86%. A projection that survives contact with the
measurement is worth more than the measurement alone - it means the
model of WHY a pin pays (each one removes a whole 4-instruction memory
shape, not merely an addressing mode) is right.

**MARGINAL per additional pin, pins 1..8:**

    83_regs_int_40    0.00 -0.91 -0.77 -0.62 -0.62 -0.63 -0.63 -0.63
    80_regs_int_08    0.00 -3.60 -3.11 -2.57 -2.64 -2.71 -2.78 -2.86
    01_while_loop    -8.28 -9.04  0.00  0.00  0.00  0.00  0.00  0.00
    07_nested_loops  -8.50 -3.09 -0.01  0.00  0.00  0.00  0.00  0.00
    14_array_subscript 0.00 -12.47 -14.25 0.00 0.00 0.00 0.00 0.00

⛔ **80's MARGINAL IS STILL MONOTONICALLY RISING** - -2.57, -2.64,
-2.71, -2.78, -2.86 for pins 4..8. Every register is worth more than
the one before it. The plateau benches are unchanged and still land
exactly at each program's hot-local count (01 at 2, 07 at 2, 14 at 3),
which is the cross-check that the curve measures what it claims.

**THE PREDICTION PIN 9 WILL TEST.** 80_regs_int_08 has 8 accumulators
plus a counter = 9 hot slots, so at 8 pins exactly ONE slot is still in
memory. Pin 9 should capture it (~-2.9%) and then 80 should PLATEAU -
its first plateau. 83_regs_int_40 has 41 hot slots and should keep
paying -0.63%/pin indefinitely. If pin 9 does NOT plateau 80, the
hot-slot model is wrong and the remaining conversion cost should be
re-argued before it is spent.

**REVISED PROJECTION for the rest.** 8 -> 13 is ~-3.1% more on 83
(5 x -0.63%) and, on 80, one more paying pin then flat. That is a
materially smaller prize than the 7 -> 13 figure quoted when the
conversion was costed, because 80 - the bench with the big numbers - is
one pin from exhausting its live values. **The cheap registers were the
valuable ones**, which is the argument for the cheapest-register-first
ordering rather than alloc_scratch's preference order.

## 2026-08-18 - #96: the ELEMENT-TIER RESERVATION, and RSI as pin 9

**WHAT.** Two changes that had to land together. `elem_scratch_reserve`
(jit.cpp) makes the pin pool withhold members until the element-store
tier is guaranteed two of its six scratch candidates; rsi then joins
`XCACHE_ORDER`, taking the int pin budget to **9** (`mylang -v`:
`jit_pins 9 ... xcache 5 caller-saved`).

**WHY TOGETHER.** `elem_scratch_plan` allocates `idx` and `val` - the
two roles the ISA does not fix - from
`ELEM_CAND = { rdi, r9, r10, r11, rsi, r8 }`, and declines the WHOLE
inline store tier to the helper if it cannot fill both. Every register
#96 admits to the caller-saved pool is one candidate fewer. rdi's
admission the day before had already starved it off-arena, patched with
a one-line per-register rule; rsi is itself a candidate, so that rule
would have had to grow a clause - and r9, rdx, rcx and rax are all in
CAND or ISA-fixed behind it.

**THE RULE.** At pool-pick time, count the candidates this run can use
for reasons other than a pin (`elem_reg_usable_nopin` - the same
predicate the emitter asks, split out so there is one rule set), split
them into "no pin can reach this" and "this pool could spend this", and
withhold spendable members least-preferred-first until two survive.
Conditioned on `run_has_elem_scratch`, so a run with no element store
pays nothing.

**MEASURED (MYLANG_JITSTATS, bench + samples + functional).**

    config                        elem_noreg   elem_reserve
    on-arena, before rsi                   0              0
    off-arena, before rsi                  0             17
    on-arena, after rsi                    0             14

The reservation absorbs exactly the pressure each admission creates and
nothing has starved with it in place. Emitted code on-arena was
**byte-identical over all 109 corpus programs** for the reservation
alone (`scripts/vdjcmp.sh`), i.e. it landed inert and rsi is what moved
it.

**TWO ROTTED SITES had to be fixed before rsi could join**, and neither
was in the audit's tidy list:

 - **`emit_store_elem` staged its arguments BEFORE the prologue** - the
   only call site in the file to invert the order. With an argument
   register pinnable that is a wrong answer: the stage overwrites the
   pin, and `emit_call_prologue` then spills the OVERWRITTEN register
   to the pin's slot. Its comment ("read before the cache regs spill")
   rested on a premise the prologue's own comment contradicts - a spill
   does not invalidate;
 - **`emit_op` did `movabs rsi, t_int` unconditionally** for
   `store_dst`, whose tag write became an imm32 in #96 step 3: dead
   code on-arena, and a pin clobber. The exact mirror of the
   `store_dst_bool` bug (there an argument outlived its loader). 3 -> 0
   such instructions over the sampled benches.

**⛔ THE SABOTAGE, AND WHY THIS NEEDED A NEW TEST.** With
`elem_scratch_reserve` returning 0 and rsi in the pool, the detector
program's inline element stores go from **80 to ZERO** - and `-rt` was
**1923/1923 GREEN**, along with all four differentials and corpus_diff.
The helper computes the same answer, so only the speed changes. Same
shape as #96 step 3's nested-store tier (emitted perfectly, reached 0
of 64).

`jit_elem_scratch_reserved` (tests.cpp) is the net: eight hot int
accumulators, a runtime bound, an element store on a loop-invariant
base so a C1 hoist claims r10/r11. It asserts the counter (`store_fast`
bumped, `elem_noreg == 0`) and carries an ANTI-VACUITY assertion
(`elem_reserve > 0`) so it reports "this program no longer exercises
the reservation" the day the pool shrinks or ELEM_CAND grows.

**TWO SELF-CHECKS ADDED with it**, both cheap and both of the
audit-table family:

 - `elem_scratch_plan` takes the opcode and ML_CHECKs it against
   `op_uses_elem_scratch`, so a third emitter that takes a plan without
   registering its op aborts BY NAME rather than silently losing its
   inline tier;
 - `HOIST_REGS_MASK` is one definition of C1's (data, count) pair, read
   by the region setup, `jit_xcache_clobber` and the reservation. Two of
   those three spelled `10` and `11` as literals.

**THE STALE EXPECTATION THIS EXPOSED** is worth recording as a
positive: `jit_xcache_pins`' hoist case expected a DECLINE off-arena
and now ENGAGES, because the reservation asks whether the run has an
element STORE and that case has only element READS (whose emitter takes
no ElemScratch). The old rule denied rdi in every off-arena run
regardless. The expectation is `true` in both configurations now, which
is also the stronger assertion - it fails if EITHER configuration loses
its last pool member.

## 2026-08-18 - #96: the tag-compare seam had a HOLE, and it cost bytes

**WHAT.** `cmp_reg_tag_via(reg, tag, scratch)` - the compare twin of
`store_type_tag_via` - and the seven `movabs r9, t_arr; cmp rax, r9`
pairs that open-coded it now route through it.

**WHY IT EXISTED.** #96 step 3's note says there is ONE tag-comparison
entry point and every tag reader must come through it. There wasn't:
`cmp_reg_tag` ML_CHECKs that a non-imm32 tag is `t_int` or `t_float`
(the only two a register holds), so `t_arr` could not use it at all,
and seven element-tier sites materialised the tag by hand instead. A
seam with a hole is filled by hand-written code, and hand-written code
is where the register names come back.

**MEASURED.** On the arena the pair is 13 bytes (`movabs` + `cmp rr`)
where `cmp rax, imm32` is 6, on every element base gate:
**98588 -> 98442 emitted instructions** over bench + samples, i.e. ~146
`movabs` deleted / ~1.4 KB of machine code. Off-arena the emission is
byte-identical (109/109) - the fallback is exactly what was there.
r9's census: 117 -> 109.

**AND THE CLOBBER MOVED TO WHERE IT HAPPENS.** `cmp_reg_tag_via` calls
`scratch(sc)` itself, on the fallback path only, so
`emit_elem_base_gate` drops its blanket `e.scratch(R9)` - which on the
arena was asserting a register that path no longer writes. That matters
beyond tidiness: once r9 is pinnable, an over-broad `scratch()` ABORTS
a legal fragment. Same rule as "a helper's register ABI is the
emitter's job".

## 2026-08-18 - vdjcmp: a difference is CONFIRMED before it is reported

**THE PROBLEM.** Twice in one session a full-corpus run reported
exactly ONE differing program - `29_str_slice_readonly`, then
`67_make_dict` - and neither reproduced. 250 same-binary runs and 200
cross-binary runs of each, six clean self-tests, and six more clean
full-corpus runs: all identical. Rate is on the order of 1 in 1000
program comparisons, and the cause is NOT yet known.

**WHY IT MATTERS MORE THAN ITS RATE.** This script is the oracle for
"my JIT change is a pure restructuring". A spurious single-program DIFF
reads exactly like a real regression - and, worse, teaches the reader
to wave the next real one away as "that flake again". That is the
failure mode the 2026-08-17 rewrite of this script already fixed once,
in its loud form (0 identical / 108 differing, for weeks).

**THE FIX, WHICH IS NOT A MASK.** On a `cmp` mismatch the script
RE-RUNS both binaries and asks again. A real difference reproduces; one
that does not is reported as `FLAKE:`, its three dumps are saved to
`./vdjcmp-flake/` (override with `VDJCMP_EVIDENCE`), it is counted in
its own column, and the script **exits 3** with a message saying the
ORACLE is at fault and this run is not a clean one either. The run is
not discarded, it is re-asked - instrument property 2 - and the
evidence needed to root-cause it is now captured instead of lost.

**IT FIRED ON ITS FIRST REAL USE** (and the evidence was then deleted
by an `rm -rf` in the same command - don't do that). Twelve subsequent
full-corpus runs have been clean.

## 2026-08-18 - #96: r9 REJOINED the pin pool (pin 10)

**WHAT.** `mylang -v` reports **`jit_pins 10 ... xcache 6
caller-saved`**. r9 is the register that shipped a wrong answer for a
day (2026-08-16/17); it is back only because every site the first
attempt missed is now an ARGUMENT instead of a name. Census 103 -> 23.

    the element READ tiers   -> elem_read_idx(e, op)
    the element STORE tiers  -> ElemScratch's sc.idx (see below)
    the CAPTURE/global chain -> ctx_chain_reg(e) + load_base/store_base
    the C1 hoist preheader   -> its t_arr compare uses RCX

The capture chain is the one that mattered: it is the site that printed
88854283473440, and **no gate could ever have covered it** - a run-level
predicate like `jit_run_blocks_xcache` keys on CALL ops, and
`LoadCaptureV` is not one. It had to become an argument.

**⛔ THE CONVERSION FOUND A LATENT BUG THAT WOULD HAVE MADE THE
ADMISSION WRONG AGAIN.** Eleven sites in the store tiers read

    load_index_r9(e, in);          /* loads the index into r9   */
    e.cmp_rr(sc.idx, sc.count);    /* ...but compares sc.idx    */

The ScratchPlan's whole promise is "the emitter is TOLD which register
holds each role" - and this role was told and ignored. Harmless while
`sc.idx` is r9 whenever nothing else can be (i.e. always), and a wrong
answer the moment r9 is pinnable, which is exactly the state this
change creates.

It hid because `load_index_r9` put the register in the NAME, so nothing
in the source visibly disagreed with `sc.idx`. **Making the register an
argument is what made the disagreement legible** - the whole argument
for the seam, in one line.

**VERIFIED.** -rt green in {debug ASan+UBSan, release TESTS +
VM_HARDENING} x {arena, MYLANG_NO_LOWMEM=1}, 1924/1924 each;
corpus_diff plain / --levers / --cold / --xrot (SIX rotations now) /
--nolowmem all 21/21; every intermediate step byte-identical on 109
programs in both configurations (the register changes are inert until
the pool takes r9); and the twelve-line historical repro prints 56640
at every rotation, where the 2026-08-16 build printed
88854283473440.

**THE RESERVATION NEEDED NO CHANGE** - r9 leaving the
guaranteed-survivor set is just a smaller starting count.
Corpus-wide on-arena: elem_noreg 0, elem_reserve 55 (0/14 at nine
pins). Because withholding walks XCACHE_ORDER backwards, r9 is handed
back to the element tier before any other member, which is where it is
most useful.

**LEFT: rdx (118), rcx (222), rax (393)** - all ISA-fixed roles
(ElemScratch's count/obj, idiv's RDX:RAX, the SIB base, the shift
count, every helper's return). Threading does not free them; each needs
its arm to spill around itself or decline when the register is pinned.

## 2026-08-19 - #96: instrumentation that perturbs nothing, and the
## build configuration I never compiled

**THE CHANGE.** `Emitter::bump_counter` takes NO register. It emits
`push rax; movabs rax, &ctr; inc qword [rax]; pop rax`, so a TESTS
counter cannot disturb any register at all. Every one of the 24 call
sites - and `bump_op` - goes through it.

**WHY, in three steps, because the first two fixes were not enough.**
The seam already existed, with a comment saying exactly why ("for sites
where rax is live"). Nineteen sites open-coded `movabs RDX, &ctr; inc
qword [rdx]` past it, and `bump_op` hardcoded rax with the register
named only inside a COMMENT - `movabs(0 /* rax */, ...)`, invisible to
a grep for RAX.

Routing them through one register-TAKING helper was fix two, and it
still left something choosing, per site, a register that happened to be
dead. These are all `#ifdef TESTS`, so a wrong choice makes the TESTS
build's register traffic **differ from the shipping build's** - the net
meant to catch a pin bug would report one the release does not have, or
hide one it does. With rdx and rax both queued for the pin pool that
was going to be wrong somewhere. **Instrumentation must not perturb
what it measures**; two bytes in a build already paying for counters is
the cheapest possible way to guarantee it. Census: RDX 119 -> 107,
RAX 394 -> 390, RCX 223 -> 218.

**⛔ AND THE PART WORTH MORE THAN THE CHANGE: `make OPT=1` HAD BEEN
BROKEN FOR FIVE COMMITS.** The `elem_noreg` / `elem_reserve` counters
added with the element-tier reservation are defined inside jit.cpp's
`#ifdef TESTS` block, like every counter there - but their increments
were not guarded, so a plain release build failed to compile:

    error: 'g_jit_elem_reserve' was not declared in this scope

Every lane I had run was `TESTS=1`: debug (`TESTS=1 OPT=0`) and
rel-hard (`TESTS=1 OPT=1 VM_HARDENING=1`), plus corpus_diff and vdjcmp
against those. `build-claude/release` - a plain `make OPT=1`, which
CLAUDE.md names as the ONLY binary to benchmark - was compiled by
nobody.

This is the `LTO=0` lesson exactly: **a configuration nobody compiles
is a configuration that is broken.** CI *does* have the lane
(`release-smoke`, `-DTESTS=0` in linux.yml, whose own comment says a
no-TESTS build "catches a declaration that escaped a TESTS guard"), so
it would have failed on push - but five commits of local verification
called itself green while the shipping build did not exist.

**THE BATTERY GAINS A LANE.** A JIT change is now verified over
{debug ASan+UBSan, rel-hard, **plain OPT=1 no-TESTS**} x {arena,
no-arena}, not the first two alone. The no-TESTS build is also the only
one that can tell a TESTS-guarded change from a real one - which
matters directly here, since the push/pop makes every TESTS-build
disassembly differ while the shipping build is untouched.

## 2026-08-19 - #96: RDX joins as pin 11, by a POSITIVE run predicate

**WHAT.** `mylang -v`: **`jit_pins 11 ... xcache 7 caller-saved`**.

**WHY THE MECHANISM IS DIFFERENT.** rdi, rsi and r9 were admitted by
enumerating their own sites against the gates. rdx cannot be: it is raw
scratch at ~100 unbracketed sites - the element tiers' COUNT role,
`idiv`'s RDX:RAX dividend and remainder, `emit_div_magic`, UnaryV,
OrdCharV, the global chain's GlobalFuncTable output - and several are
ISA-forced, not habits an allocator can talk out of.

Enumerating what DOES touch it is the r9 mistake, where a missing entry
is a silent wrong answer. So `run_may_pin_rdx` inverts the question the
way `run_needs_float_tag` does: rdx is spendable only in a run made
ENTIRELY of ops positively established never to write it. An unlisted
or brand-new opcode keeps rdx out - the failure direction is a lost
pin, never a wrong answer. The list is the B1/B2 specialized int family
plus int loop control, compares, ReturnV and Halt, minus
IntModRI/IntAddModRI. Verified rdx-free by reading every shared path
those ops reach: store_dst, write_slot, load_operand, op_rr, exit_pc,
raise_unless, emit_raise, flush_cache, frag_ret, emit_epilogues.

**⛔ REACH WAS ZERO ON THE FIRST ATTEMPT, AND ONLY THE "PROVE THE CODE
RAN" RULE CAUGHT IT.** `-rt` was green, corpus_diff was green, the
admission looked done - and **not one program in the corpus spent rdx**.
The whitelist omitted `ReturnV`, and a leaf body is ONE run ending in
ReturnV, so the predicate refused every fragment there is. Without the
counter check this would have been reported as a register gained.

ReturnV and Halt belong for the same reason `jit_run_blocks_xcache`
omits them: `emit_ret_native` uses rdx freely (16 sites) but its SECOND
LINE is `flush_cache()`, so every pin is already in memory. With them
listed, rdx is spent at **107 pin entry loads across 36 programs**.

(Count the ENTRY loads - `mov rdx, <slot>` - not the flushes. A first
pass counted `mov <slot>, rdx` and reported 264/65, which also matches
every element-tier store that happens to write through rdx. An entry
load into rdx happens for a pin and nothing else.)

`MYLANG_RDXDBG=1` now names the op that refused, so widening the list
is evidence-driven rather than guesswork - the instrument that would
have made the zero-reach obvious in one run.

**⛔ AND THE IMPROVEMENT ATE THREE TEST SHAPES - TWO OF THEM
SILENTLY.** `jit_two_address` had three cases built on "TEN
accumulators, so they OUTNUMBER the pin pool", true when the pool held
eight. At eleven every accumulator gets pinned:

 - the memory-form case failed loudly ("the tier never engaged");
 - **the imul DECLINE case passed** - it asserts `hits == 0` about a
   tier that could no longer fire at all, which its own comment had
   warned about in a different form ("a decline case that cannot fail
   is not a decline case");
 - the tag-survival case likewise.

Same failure `jit_telide_c3` had when r9 widened the pool. The fix is
the recorded one: `two_addr_prog()` DERIVES the accumulator count from
`jit_pin_budget()`, and takes the expected output from the TREE-WALKER
rather than a literal - a derived N changes the sum, so a hardcoded
answer would only move the staleness one field to the right.

**THE LIMITATION, WRITTEN DOWN RATHER THAN PAPERED OVER.** rdx is a
NARROW pin: available in dense scalar int loops and nowhere else, since
any element or div/mod op in the run refuses it. Widening it means
threading the element tiers' `count` role the way `idx` was threaded
for r9 - the eleven fixed-pair accessors deleted the day before are the
prerequisite, and every one of those sites now takes its register as an
argument.

## 2026-08-19 - the macOS lane, failing for the SAME reason as 2026-08-05

`two_addr_prog` - the budget-derived accumulator generator added with
rdx - was defined at file scope in tests.cpp, OUTSIDE
`#if ML_JIT_SUPPORTED`. Its only caller, `jit_two_address`, is inside
one. So on any platform without the native tier nothing calls it, and
macOS clang refused the build:

    src/tests.cpp:24884:1: error: unused function 'two_addr_prog'
                                  [-Werror,-Wunused-function]

CLAUDE.md records this verbatim under *EMITTER-ONLY CODE LIVES INSIDE
`#if ML_JIT_SUPPORTED`* - same warning, same lane, same cause, from
2026-08-05 - **and prescribes the local check that finds it**: flip
jit.h's `#if defined(__linux__) && defined(__x86_64__)` to `#if 0`,
build with BOTH g++ and clang++, run `-rt` (the JIT tests self-skip),
restore; and build once with CMake, which is what CI runs.

I skipped that check. Six local lanes were green - debug, rel-hard,
plain release, corpus_diff, vdjcmp, the arena matrix - and not one of
them compiles the off-platform path, exactly as the rule says.

Now verified, and this is the shape the battery should keep:

    non-JIT g++      build + -rt 1924/1924
    non-JIT clang++  build + -rt 1924/1924
    CMake Debug      build + -rt 1924/1924   (what CI runs)
    clang JIT        build + -rt 1924/1924

**The generalisation worth keeping: a `-Werror` diagnostic that only
one PLATFORM can see is one no local lane will find**, the sibling of
`LTO=0`'s "a warning only one build configuration can see is a warning
nobody sees" and of the plain-`OPT=1` break found the same day. Both
were caught by CI; both should have been caught before the push.

## 2026-08-19 - TWO SHIPPED JIT BUGS, both found by ONE new corpus file

Neither is a #96 regression. Both were sitting in the tree, both were
invisible to `-rt`, `corpus_diff`, all four fuzzers and `vdjcmp`, and
both were found within a minute of adding
`tests/functional/16_elem2_fused.my` - a file written for an
INFRASTRUCTURE reason, to close a hole in the byte-identity oracle's
CORPUS. That is the whole entry: the coverage hole in the instrument
was also a coverage hole in the correctness net, and closing the first
closed the second.

### The hole

`scripts/vdjcmp.sh` compares emitted code over bench/my + samples +
tests/functional. Of those 109 programs, **zero emitted
`LoadElem2Float`** - so neither float arm of the fused nested read, nor
its row-slice arm, was covered by the oracle every emitter refactor is
verified with. The `elem2:` cases in tests.cpp do exercise them, but
they run IN-PROCESS: an in-process test proves the VALUES, only a corpus
program proves the BYTES.

`05_elem_tiers.my` looks like it covers this and does not. It reads
`m[r][c]` in a `c` loop, so the row is loop-invariant, LICM hoists it,
and no fused op is emitted at all. **The outer index has to vary with
the INNER loop** or this tier is unreachable - the vacuous-test trap,
in the exact clothes CLAUDE.md lists.

### Bug 1 - a slice read the WRONG ELEMENT under the float op

`emit_load_elem2_inline` ended its float branch with `return`. The two
SLICE arms are emitted after it, so on the float path `j_oslice` and
`j_rslice` - forward `jne rel32`s already written into the fragment
with a 0 placeholder - were **never patched**. An unpatched
`jne rel32=0` neither declines nor faults: it falls through into the
NON-slice path, which reads the slice's shared object (its PARENT's
storage) and indexes it without the slice's `off`.

    var r = []; ...; push(mfs, r[2:14]);
    for i, j:  srs += mfs[j][i];

    -tw  1287.50      -nj  1287.50      jit  1237.50

- `off` too small on every element, at BOTH levels (a slice outer read
the wrong ROW the same way), under the float op only. An `if`/`return`
where an `if`/`else` was meant. The int twin patches both arms and was
always correct, which is exactly why the shapes read as covered.

Fixed by making it an `else`. The counters confirm the diagnosis rather
than merely the values: `elem_slice_fast` goes 100 -> 200 on the new
test (the float slice row now takes the slice arm) while `elem2_fast`
drops by the same 100 (it was being counted by the arm it wrongly fell
into).

### Bug 2 - a rel8 jump span that GROWS WITH THE PIN BUDGET

Adding one more accumulator to the new test turned it into an abort:

    Assertion `d >= -128 && d <= 127' failed   (Emitter::patch8)

`emit_ref_check` returned a SHORT `jb`, and all three of its callers
put a helper call between that jump and its patch. A call carries the
whole `emit_call_prologue`/`emit_call_epilogue` bracket, **whose length
is the number of live caller-saved pins** - the quantity #96 exists to
increase. So this was dormant at a small budget and became reachable as
the budget grew: it aborts from `MYLANG_JIT_MAXPINS=8` up, and is
CLEAN at 4 and 6.

**At `ASSERTS=0` there is no abort at all** - the displacement
truncates and the jump lands inside an instruction. The shipping
release configuration is the one with no net.

`patch8`'s own comment had named this hazard, precisely, for a year:
*"use j32 for any span that can grow (an exit_pc carries an N5 flush, a
helper call its whole prologue/epilogue)"*. It was a comment, so
nothing enforced it, and the one site that broke it was written anyway.

### Why no audit could have found bug 2, and what replaces the audit

A script that reads the source between each `j8` and its `patch8`
looking for a call reports **"0 calls in span" for the site that
crashes** - because the call is not written there. It is inside
`emit_put_int_call`, one function call away. Same shape as a register
hidden in a method name (the sixth audit-table shape): the thing being
audited for is behind a NAME, so a text scan cannot see it.

The check therefore lives in the EMITTER, not in a script.
`Emitter::n_prologues` counts the call prologues emitted; `j8` records
the count at the jump; `patch8` requires it to be unchanged and says
so by name. Debug-only (`#ifndef NDEBUG`), so a release build has
neither member.

**It fires on `samples/gcd`.** Reintroduce the short jump and the
smallest sample in the repo aborts immediately, and `-rt` dies on its
first JIT test - where the real defect needed a program big enough to
push one span past 127 bytes AND a pin budget of 8 or more. That is
the property to want: the guard triggers on the SHAPE, not on the
shape plus a size coincidence.

### Cost - and a THIRD instrument failure, in the measurement itself

The fix makes three `jb`s near instead of short: +4 bytes at each
ref-listed scalar store's cold-arm entry, so 108 of 109 corpus
programs' emitted code changed. Ir at `OPT=1 ASSERTS=0`, callgrind,
scale 1:

    01_while_loop +0.003%   03_int_arith +0.001%   43_sieve +0.001%
    14_array_subscript +0.002%   46_matrix_mult +0.002%
    58_structs +0.001%   64_struct_create +0.001%

i.e. nothing, and necessarily so: `jb rel8` and `jb rel32` are both ONE
instruction, so only the once-per-compile emission cost moves. A
restructure that puts the fast arm first would recover ~1 byte; it is
an OPTIMIZATION and is deliberately not bundled with a correctness fix.

⛔ **THE FIRST VERSION OF THAT TABLE READ +1.67% ON 46_matrix_mult, AND
IT WAS A STALE BUILD DIRECTORY.** `build-claude/perf` existed from an
earlier session, built with different flags; `make BUILD_DIR=...` does
not rebuild on a flag change, so the "current" binary was partly
someone else's configuration. The tell was that the number made no
sense - the emitted instruction SEQUENCE was identical, 3322 lines
each, and identical code cannot execute more instructions - and
chasing it found the stale binary emitting the REGISTER tag form (9
`movabs r9, <array-tag>` where the clean build emits `cmp rax,
<array-tag>` as an imm32). Clean-built, the delta is the table above.

**RULE B1 covers `build/` and a forgotten `--mylang`; it does not
cover a REUSED `build-claude/<lane>` whose flags have changed, which
is the same failure with the same signature - a plausible number
measured from the wrong subject.** Build measurement lanes FRESH
(`rm -rf` first), on both sides, and delete them when the measurement
is done. Note also that the debug pair was byte-identical throughout,
so `vdjcmp` on debug binaries would never have shown this: the stale
artifact was release-only.

## 2026-08-19 (2) - A THIRD shipped bug: the divmod cold gate SIGFPEs

Same file, same hour, same cause as the r9 admission that shipped a
wrong answer for a day: **a register named in a place no audit for the
register can see.**

`emit_store_elem2_inline`'s #103 divisor gate - the arm that must
decline `d == 0` and `INT_MIN / -1` to the helper, so they raise a
CATCHABLE `DivisionByZeroEx` - was five HAND-ENCODED byte sequences:

    e.u8(0x48); e.u8(0x8D); e.u8(0x57); e.u8(0x01);  /* lea rdx,[rdi+1] */
    e.u8(0x48); e.u8(0x85); e.u8(0xFF);              /* test rdi,rdi    */
    e.u8(0x4A); e.u8(0x8D); e.u8(0x14); e.u8(0xCA);  /* lea rdx,[rdx+r9*8] */
    e.u8(0x48); e.u8(0x3B); e.u8(0x50); <data_off>;  /* cmp rdx,[rax+d] */
    e.u8(0x4C); e.u8(0x39); e.u8(0xCA);              /* cmp rdx,r9      */

`rdi` is the DIVISOR and `r9` is the INDEX - and both are ALLOCATED
`ElemScratch` roles. `elem_scratch_plan`'s `pick()` prefers rdi/r9 and
falls back to the next `ELEM_CAND` member whenever the preferred one is
pinned. **That is not rare**: a probe asserting the roles never leave
their defaults trips on `bench/my/43_sieve.my` at every pool rotation,
and inside `-rt`.

With `sc.val` moved, the gate tested a register that does not hold the
divisor, so `m[j][i] /= 0` walked past it into the native `idiv`:

    -tw  737555 1 22080      (DivisionByZeroEx, caught)
    -nj  737555 1 22080      (DivisionByZeroEx, caught)
    jit  Floating point exception (core dumped), exit 136

RULE 1 and RULE 2 at once, plus a crash on a valid program (#137).

FIXED by giving the block the ROLES and adding the four generic
encoders it should always have had - `lea_base`, `lea_elem_q`,
`cmp_reg_base`, `load_base0`, over a shared `emit_modrm_disp` that
picks mod=00/disp8/disp32 so the emitted bytes are IDENTICAL to the
hand-written ones when the roles sit on their defaults. Verified: the
whole corpus is byte-identical except the new test, whose one diff IS
the bug -

    -  lea rdx, [rdi+0x1]        <- tests the wrong register
    +  lea rdx, [r10+0x1]        <- `sc.val` was allocated to r10

**WHY THE EXISTING NETS COULD NOT SEE IT, and this is the part to
keep.** The shape needs THREE things at once: a nested COMPOUND
`/=`/`%=`, a divisor that is actually 0 or -1, and enough pin pressure
that the allocator moves `sc.val` off rdi. No corpus program had all
three. Measured with the defect reintroduced:

    -rt                      1924/1924 GREEN
    corpus_diff (old corpus)          GREEN
    corpus_diff (+ the new file)      22/23 - and a SIGFPE

`tests/functional/17_elem2_divmod_roles.my` pins it. **Its eight
accumulators are load-bearing, not padding** - with two or three the
roles stay on rdi/r9 and the file passes with the defect in; the count
must stay at or above the pin budget. Keep the loop bound RUNTIME, too
(a literal bound lowers to a different loop form - shape-eater #7).

**THE GENERALISATION, now three-for-three in one day:** every one of
these bugs was a fact the code stated in a form no scan could read - a
register in a byte literal, a register in a method name, a call behind
a helper's name, a rule in a comment. The fix is always the same shape:
make the thing an ARGUMENT, or make it a MACHINE CHECK. Do not write it
down and hope.

## 2026-08-19 (3) - a FOURTH: the boxed truth test compared against a
## register nobody loads

`JumpUnlessTrueV`'s inline fast path decides int-vs-bool-vs-slow from
the value's Type*, and its first compare was hand-encoded:

    e.u8(0x48); e.u8(0x39); e.u8(0xF0);   /* cmp rax, rsi (t_int) */

rsi is materialised with `t_int` by exactly one line, and that line is
guarded:

    if (!jit_tag_is_imm(jit_layout().t_int) && !e.reg_holds_pin(RSI))
        e.movabs(RSI, ... t_int);

- i.e. **only OFF the low-address arena**. On the shipping
configuration the dump settles it:

    default (arena)   cmp rax, rsi: 1     movabs rsi,<int-tag>: 0
    MYLANG_NO_LOWMEM  cmp rax, rsi: 1     movabs rsi,<int-tag>: 6

So on-arena the int arm compared against whatever rsi happened to hold.
The practical cost is a DEAD FAST PATH - every boxed truth test on an
int fell through to the `jit_is_true` helper call - and the tail risk
is a wrong answer, since a coincidental match would run the fast path
(which reads the payload as a truth value) on a non-int.

This is the class CLAUDE.md already names from 2026-08-18: **"an
optimization that makes a register UNNECESSARY must not leave an
argument behind claiming it is still LOADED."** That entry describes
`store_dst_bool` passing RCX as the register holding `t_bool` after the
`movabs` was deleted. Same shape, opposite direction (a READ, not a
store), and it survived the seam cleanup because THIS SITE NEVER WENT
THROUGH THE SEAM - it was raw bytes.

FIXED by routing it through `cmp_rax_tag(t_int, RSI)`, which already
existed for exactly this: `cmp rax, imm32` on the arena, `cmp rax, rsi`
off it. The other hand-rolled tag compares went with it -
`cmp_reg_tag_via` for t_bool, t_none and the two t_struct gates - and
the boxed-op arm's hoisted `movabs RCX, t_int` + three bare compares
became three seam compares (shorter off-arena too: 3xN vs 10 + 3xN).

**`cmp_rax_rcx()` IS DELETED - it was the FIFTH fixed-pair accessor,
and the 2026-08-17 sweep that removed `cmp_rax_r8` / `cmp_rax_rsi` /
`cmp_rdx_r8` / `cmp_rdx_rsi` missed it.** Nine call sites, invisible to
`grep RCX`. Its two genuine value-compare users take `cmp_rr(RAX, RCX)`.

MEASURED (callgrind Ir, `OPT=1 ASSERTS=0`, fresh lanes both sides):

    a boxed truth-test loop   -19.95% on-arena, -19.94% off
    66_dyn_foreach            -1.19%
    79_dyn_float              -1.19%
    42_exceptions             -0.08%   56_sieve_bool  -0.06%
    43_sieve                  -0.05%   46_matrix_mult -0.02%
    01/03/06/54/57/58/64/76   -0.00x%  (flat)

Nothing regressed. The synthetic probe is 20% because it is nothing
BUT boxed truth tests and boxed arithmetic; real programs mix them with
work. 33 of 111 corpus programs' emitted code changed on-arena, 26 off.

**Note 06_if_branch does NOT move**: its conditions are M8-typed, so
they never reach this op at all. The benches that move are the `dyn`
ones - which is the reach statement this fix needs, and the reason a
"boxed" tier's regressions hide from a corpus of typed programs.

## 2026-08-19 (4) - THE DISASSEMBLER: two missing opcodes, three SILENT
## "I don't know" markers, and an objdump oracle

Maintainer requirement, and it is absolute: **no instruction the JIT
emits may be undecodable, or wrongly decoded, by `-vdj`.** Everything
downstream - vdjcmp, every claim about emitted code, reading a dump by
eye - rests on it.

It was not met. Three separate problems.

### 1. The self-check could not see the failure it exists for

`DUMP IS UNRELIABLE` counts `.byte` lines: bytes we KNOW we failed on.
It says nothing about a byte sequence we decode CONFIDENTLY AND
WRONGLY, which is the failure that cost weeks in August (the SIB arm
that never consumed its displacement). **A decoder cannot check
itself** - the decoder is the subject.

### 2. Three markers that looked like output and were not

    case 0xF7: o << ((regf & 7) == 7 ? "idiv " : "f7/? ") << rm;
    ...                              : sub == 6 ? "push " : "ff/? "
    else { o << ".0f 0x" << hex2(o2); }

`f7/? rdx` is not a mnemonic and not a `.byte`. It is not counted, so
the banner stays silent - AND it claims a LENGTH the decoder has not
earned, so everything after it is read at the wrong offset. Every path
now lands on one `undecoded:` label that rewinds `p` and emits `.byte`.

**That single change retroactively armed the existing `-rt` check.**
With the F7 gap reintroduced, `jit: -vdj decodes every emitted form`
now FAILS (1923/1924); it passed for the gap's entire lifetime, because
`f7/?` was not a `.byte`.

### 3. Two genuinely missing opcodes, on 284 corpus sites

`F7 /3` (`neg`) and `F7 /5` (`imul`) - the div-magic sequence. Only
`/7` (idiv) was decoded. The whole group is now handled (/0 test, /2
not, /3 neg, /4 mul, /5 imul, /6 div, /7 idiv).

### The oracle: `scripts/disasmcheck.py`

`MYLANG_VDJ_HEX=1` makes `-vdj` print each instruction's raw bytes;
the script hands each fragment to **objdump** and compares

  * **BOUNDARIES** - objdump's instruction lengths must equal ours.
    This is the desync check, and the one that matters most: a wrong
    length makes every later mnemonic wrong.
  * **MNEMONICS** - normalised, because the two spell operands
    differently on purpose (`-vdj` prints slots by name and baked
    pointers as `<addr>`). Only the opcode and register/memory shape
    are compared.

objdump is a development-time tool a script invokes, like python3 in
the other scripts - not a build or test dependency.

**WRAPPING IS PREVENTED WITH `-w`, AND THEN CHECKED FOR.** `-w`
(--wide) plus `--insn-width=16` keep every instruction on one line -
the direct fix for the two false alarms above, which both came from
wrapping and both accused the decoder. A continuation line is no longer
skipped, it is COUNTED and reported with its own wording (*THE ORACLE
is misconfigured, not the decoder*) and its own exit code (2, a setup
error, not 1, a decode mismatch). Silently tolerating it would leave
the script working BY ACCIDENT on an objdump whose wrapping the flags
failed to suppress - a workaround living in the consumer, the trap one
level up from the one this script exists to close. Watched: dropping
the flags reports 28,920 wrapped lines and exits 2. It also refuses to
run at all when objdump is absent, rather than reporting a vacuous
pass.

**RESULT, and this is the answer to the requirement:**

    2,209,682 instructions   2,884 fragments
    boundary errors 0        mnemonic errors 0

over the whole corpus x {both arenas} x {7 pin-pool rotations} x
{5 pin budgets} - the axes that change WHICH encodings get emitted.
Watched failing: reintroducing the `/5` gap gives 46 boundary errors,
fires the banner, and fails `-rt`.

### And one EMITTER defect it found

`load_elem_sd` / `store_elem_sd` passed `w=true` to `rex_sib`, but
`movsd` (F2 0F 10/11) has a FIXED 64-bit operand size and REX.W is
architecturally IGNORED - so every float element access carried a
prefix bit that does nothing (objdump renders it `rex.WX`). Now the
REX byte is emitted only when R/X/B needs it, which also makes the
instruction a byte shorter when none does.
**`cvtsi2sd_elem` deliberately KEEPS `w=true`**: there REX.W selects
the 64-bit integer SOURCE, and clearing it would silently truncate.

## #96 (c) THE REGISTER-STATE TRACKER, and the rcx admission it earned
## (2026-08-20)

**What it is.** An emit-time guardrail inside `Emitter` (`wrote()`,
`trk_push`/`trk_pop`, `trk_read_pin`, `assert_no_borrow`,
`op_boundary`, `PinMach`, `BorrowSuspend` - declared beside `RegAlloc`).
Every encoder that writes a GP register reports it first - possible
only because batches 3-6 made every encoder take its registers as
ARGUMENTS - and the report is checked against the live register state:
which registers hold pins, which are borrowed, whether emission is
inside declared pin machinery, a call bracket, or the post-flush
dead-pin window. A violation aborts DURING JIT COMPILATION with the
register, vm pc and opcode named. It emits nothing, so the emitted
bytes are identical with it on or off; it runs in every ASSERTS build.

**Why it exists.** Every prior instrument was REMOTE from this failure
class: the census reads the SOURCE, the admission survey reads
DECLARATIONS (`scratch()` calls - a site that simply writes a register
declares nothing), and the nets read the final ANSWER. The rcx
admission produced four hand-root-caused bugs that way; the tracker
then found the REAL list in one -rt run each:

  - `emit_exc_stamp` staged packed Locs through rcx on the conveyance
    arm - a thrown-and-caught exception flushed a SOURCE LOCATION as a
    pinned slot's value (16_elem2_fused's wrong sums were line:col
    pairs);
  - `bump_divmagic` wrote movabs(1, ...) with RCX in a comment -
    invisible to the census, the `= RCX` grep and the `movabs(RCX`
    grep alike;
  - `ForStepElemInt` built its ElemRead by hand - the FIFTH site,
    missed when the other four moved to `elem_read_plan`;
  - `PushHandler`/`PopHandler`/`SetPend`/`EndFinally` and emit_op's
    tmp/cpy staging used rcx undeclared;
  - the element divmod gate read the dividend via a hand-encoded
    `mov rdx,[rcx+r9*8]` - both roles HARDCODED, so a real INT_MIN/-1
    dividend was not declined and reached the raw idiv: a SIGFPE
    where the language must throw;
  - `StructFieldAddInt` staged through rcx after the epilogue reload;
  - `op_elem_scratch_roles` still said "rcx/rdx are forced" - the
    audit-table trap, caught by its consumer: the reservation
    undercounted and the elem tier declined silently at full pressure.

**The states, each learned from a false or missed abort:**
  - a CALL BRACKET (prologue..reload): caller-saved pins are safe in
    memory - their registers are free scratch. The bracket CLOSES at
    the epilogue's reload, not at its end: the tag/flit
    re-materialisations after it run with pins LIVE.
  - a BORROW (push of a pinned register): the borrower owns it; pin
    machinery touching it aborts; `read_slot` resolving to a borrowed
    register the borrower overwrote aborts. Borrows are ORDERED (pops
    must reverse pushes) and may not nest per register.
  - POST-FLUSH: pin registers are dead until a reload; writes are
    repurposing, and a later `read_slot` through a repurposed register
    aborts. The state is PER-OP - a run may contain a terminal op
    mid-sequence, and the next op is entered by jump with pins live.
  - a RAISE ARM (`BorrowSuspend`): a divergent arm that leaves the op
    restores open borrows ON THAT PATH (pops emitted, state preserved
    for the fall-through); `exit_pc` refuses an unsuspended borrow.
  - an IN-OP SLOW ARM entered from inside a window needs a
    COMPENSATION stub (`pop_bytes` at its head) - two entry stack
    shapes cannot share one patch point (see the fused int arm and
    PushHandler's grow).

**The admission.** rcx is pin 12 (XCACHE_ORDER gains 1, placed last;
MYLANG_JIT_XROT sweeps EIGHT rotations). With it admitted: -rt
1925/1925 in all five modes, corpus_diff 24/24 plain and at all eight
rotations, objdump 164,996 instructions clean. WATCHED: reintroducing
the un-borrowed exc stamp aborts by name in seconds, where it
previously shipped as an unattributable wrong answer.

**The tracker's stated blind spots:** raw `e.u8()` byte sequences
(~20 remain, see plans/jit-registers.md (y)) bypass `wrote()`; jumps
patched into arms emitted after a window closes are checked by the
compensation-stub CONVENTION, not by the model.

## #96 (c) addendum: THE PER-ROLE RESERVATION, and the balance rules
## the off-arena lane forced (2026-08-20)

The rcx admission's tail was three off-arena failures, each of which
hardened a different layer. All were found by the nolowmem lane +
the xcache tests, not by any value differential - the failure mode in
every case was a SILENT DECLINE (the fragment or its tier vanishes,
the answer stays right, only "engaged 0" says anything).

**1. Scratch may never be an UNSAVED callee-saved register.**
`alloc_scratch` ranked by `gp_weight`, which calls callee-saved
cheapest - right for PINS (frag_entry saves them), exactly wrong for
transient scratch: with rcx denied by the reservation, the hoist
re-derive was handed **r14 unsaved**, and the fragment returned to
`jit_call_sync_core` with the C caller's frame base destroyed - a
one-frame-up corruption invisible to every value oracle (ASan caught
it as a wild frame access, off-arena only, because only there is rcx
scarce enough). Fixed twice over: `alloc_scratch` excludes the
callee-saved set outright, and the tracker gained the rule "a write
to a callee-saved register not in `e.saved` aborts" - so the next
path to one fails by name at compile time.

**2. A borrow does not honour `denied` - only `exclude`.**
`any_capable` consulted the run's denied mask; off-arena the tags +
the reservation + the caller's exclusions covered all sixteen
registers and the re-derive found "no register CAPABLE". Wrong
premise: denied protects what a PIN would destroy, and a push/pop
BORROW preserves it. Borrows now ignore denied.

**3. The reservation is PER-ROLE, not a count** (`ElemRoleSig` +
`elem_scratch_reserve` rewritten). The scalar count failed in both
directions in one day, all watched via the xcache cases off-arena:
  - the CAPTURE/GLOBAL chain (`ctx_chain_reg`) scans ELEM_CAND ONLY,
    so a withheld rcx satisfied the count while the chain starved -
    `LoadCaptureV` returned false, `emit_ok` dropped the WHOLE
    fragment, and every max-pin closure loop ran interpreted at all
    eight rotations ("engaged 0" was the only symptom; the earlier
    per-rotation failure lists were a grep artifact - always read the
    full failure count);
  - a role whose preferred register is ALREADY un-pinnable (rdx under
    `run_may_pin_rdx`, r9 under a clobber) needs no withhold, and
    counting it starved the POOL: the hoisted-read shape withheld
    three registers where its plan needed one, leaving nothing to pin.
The rewrite re-runs each present plan's OWN preferred-then-ELEM_CAND
scan (the signature per opcode from `op_elem_role_sig`; the preferred
registers read from `ElemScratch`/`ElemRead` defaults so they cannot
drift) and withholds exactly the register each unsatisfied role will
look for. Rotation-independent by construction - it consults
preference lists, never XCACHE_ORDER - which the XROT axis requires
(a rotation-dependent withhold ate r8 at two rotations).

**Also in this batch:** per-op NET PUSH DEPTH (`trk_pushes` - a path
that leaks emitted stack aborts at the op boundary; `pop_bytes`
compensation stubs are exempt, being per-path restores of a shared
push), and `frag_entry` classified as machinery (its pushes balance
against frag_ret's teardown, not within an op).

**State after:** -rt 1925/1925 on-arena AND off-arena, in gcc dbg,
clang dbg, rel-hard (VM_HARDENING=1 OPT=1) and CMake Debug;
corpus_diff 24/24 plain, all 15 levers, all 8 rotations in BOTH
arenas; disasmcheck clean in both arenas; driver_checks green;
non-JIT builds (gate forced 0, g++ + clang) 1925/1925; LTO=0 green.

## #96 rax - THE 13TH AND LAST REGISTER (2026-08-21)

**What.** rax joins XCACHE_ORDER (last; MYLANG_JIT_XROT now sweeps
NINE rotations, jit_pin_budget() = 13), admissible per RUN through two
gates that fail closed independently:

 - **the SHAPE gate** (`run_may_pin_rax`, DELETED 2026-08-22 by the endgame Phase A entry below; was consulted by
   jit_xcache_busy): a positive whitelist in run_may_pin_rdx's mould
   but per SHAPE, not per opcode - the specialized int arith family in
   ACCUMULATOR form only (target == a_slot), ForLoopStep / IntAddStep,
   Jump, LoadImmInt, JumpUnlessIntCmp, and ReturnV / Halt on the
   flush-first argument. Everything else stages through rax somewhere:
   the survey (scripts/rcx_admission.sh 0 under the tracker's new
   MYLANG_REGTRACK_REPORT mode) measured ~2400 hits over ~25 opcodes,
   so there was never a site-by-site fix - only a run gate.
 - **the COVERAGE gate** (at the pick, before assignment): the listed
   forms are rax-free only on their PINNED paths, so every listed op's
   target must be in `hot`; one unpinned target means one generic-arm
   op staging through rax. Denial pops the coldest pick - the budget
   shrinks by one, nothing is un-assigned.

Forwarding is the third piece: lever A travels IN rax (the producer
adapter is `mov rax, pin`), so the pairing site refuses to arm when
rax holds a pin. Nothing is lost - rax is granted only to fully-pinned
runs, where the reload forwarding saves does not exist.

**Two emit improvements landed as prerequisites, both standalone
wins on every pinned operand independent of rax:**
 - LoadImmInt with a pinned target emits `movabs pin, imm` directly
   (was movabs rax + write_slot);
 - JumpUnlessIntCmp with a pinned `a` compares in the pin
   (cmp_reg_imm / cmp_rr2 / cmp_reg_slot - ForLoopStep's three forms),
   dropping both the rax load and the `b` staging. Every `for` loop
   opens with this op, so without it the shape gate refused every
   loop in the corpus (the reach probe read rax_pin = 0 - the
   "prove the code ran" rule caught it before it shipped hollow).

**Reach, honestly:** g_jit_rax_pin (JITSTATS `rax_pin`) is bumped by
the emitted entry of a fragment that spent rax. It engages on
fully-pinned accumulator kernels - 13 int slots on-arena, and NOT
off-arena, where rsi carries the t_int singleton and the budget is 12
(the jit_rax_pin_test reach case's `want` encodes the asymmetry). A
reduction tail must be written accumulator-form (`s0 += s1; ...`);
`return s0+s1+...` lowers into TEMPS, which are never pinned, and the
shape gate refuses the run. This is a NARROW reach and the admission
is the MODEL completing (13 of 16, every non-reserved register), not
a perf claim - MAXPINS already measured the marginal pin near zero.

**Watched, all three:** the coverage gate disabled aborts the
14-accumulator case at LoadImmInt ("write to a PINNED register", r0);
the shape gate disabled aborts the IMMEDIATE-count shift case at
IntShrRI - note the variable-count shift case CANNOT catch this (its
count is a 14th slot, so the budget arithmetic denies rax anyway and
the sabotage stays green; the immediate form adds no slot and is the
distinguishing shape); the tracker's report mode is what turned the
survey from one-abort-at-a-time into a worklist (and its probe runs
now carry timeouts - report-mode code is genuinely wrong and a
clobbered loop counter span forever on the first rax survey).

## #96 INCREMENT 1 - THE SPILL-EXTENDED HOT SET (2026-08-21)

**What.** The pick ranks more candidates than the register budget; the
overflow past the 13 pins is homed in bare 8-byte NATIVE-STACK slots
([rbp+spill_off(k)], the machinery landed inert earlier in the arc),
up to MAX_SPILL_HOMES = 16 (MYLANG_JIT_MAXSPILL caps for measurement;
MYLANG_JIT_OFF=scache is the kill switch; MYLANG_SPILLDBG=1 names the
homes). A home follows the PIN contract exactly: seeded at every entry
(head + stubs), flushed type+payload to the frame slot at every exit
(the interned-epilogue states carry it), snapshot/cleared/restored at
the cache barriers, and its qualification is inherited from the same
ranked pick, so the bad()-rule exclusions hold. Unlike a caller-saved
pin it needs NO call-site spill/reload - the stack survives helper
calls - so homes live in call-bearing runs the caller-saved pool must
decline. `scache` joined the four-function cache-state family as its
FIFTH member (CacheState/is_empty/snapshot/restore/clear + the
interning comparison + the epilogue swap - the "&&-over-a-family"
sites, all of them). g_jit_scache (JITSTATS `scache`) is the
execution proof.

**The three bugs the build found, each a general lesson:**

 1. **The flush shuttle clobbered the RESUME PC.** flush_cache never
    wrote a register before; the scache flush shuttles through rax,
    and the exit protocol is `mov eax, pc; jmp <epilogue>` - the
    epilogue's flush destroyed the pc and the interpreter dispatched
    at a wild opcode (190, off a 128-entry table). The shuttle is now
    push/pop-wrapped, which also covers rax-as-a-pin and the mid-op
    barrier flushes. THE RULE: a flush path that gains its first
    register write must audit every caller for values riding THROUGH
    the flush.
 2. **`load_slot_idx` consulted creg by hand** and fell to the frame
    for everything else - a stale read for a spill-homed element
    index (the reservation test summed a[k-at-entry] sixteen times).
    It routes through read_slot now, THE one resolver that knows all
    three homes. The ctor-plan field read had the identical shape.
 3. **MoveV's cache-aware source read the frame raw** for a homed
    source - the 24-byte boxed copy - so a homed `s` printed <none>
    (the tag half) and a homed catch counter lost its increment. The
    spill-homed source is a proven int by the pinned-source argument
    and now reloads + stores as the ordinary int.
    THE PATTERN across 2 and 3: "cache-aware" used to mean
    "creg-aware", and every such site is a stale read the day a slot
    can be homed in something that is not a register. read_slot /
    write_slot are the resolvers; a site consulting creg directly
    must pair it with cspill or route through them.

**Measured honestly (callgrind, OPT=1 ASSERTS=0, vs pre-inc-1):** Ir
FLAT (1.0001x) on 80-85_regs_*, 46, 03; D1 misses flat too. The
reason is worth recording: the two-address family had ALREADY made
the accumulator's frame RMW tag-free and single-instruction, so a
home trades equal instruction counts - and 40 slots x 48 bytes is 30
cache lines, comfortably inside L1, so the 8-byte-stride density
argument has nothing to bite on at this working-set size. Increment 1
is the SUBSTRATE: the location map {register | spill home | frame}
that live-range reuse (increment 2) needs for its evictions, plus
call-transparent homes, landed at zero measured cost.

## #96 INCREMENT 2 - LIVE-RANGE REUSE (2026-08-21)

**What.** One register serves several slots whose in-run live ranges
are disjoint. `jit_share_plan` computes each hot slot's interval
(first to last touch, `jit_op_slot_refs`) and chains overflow picks
onto registers whose occupied span is disjoint - in EITHER direction,
because the ranking is by use count and a late phase can out-rank an
early one (watched: t2 took the pin and loop1's `i` became overflow in
the exact shape the feature exists for; the BACKWARD chain makes the
early slot the register's ENTRY occupant by editing the hot list, and
the seam installs the pin at its own lo). At a SEAM the occupant is
evicted (type+payload to its frame slot) and the next installed.
Lever `rshare`; MYLANG_JIT_MAXSHARE caps; MYLANG_SHAREDBG=1 narrates
the plan; g_jit_range_share (JITSTATS `range_share`) is bumped by the
EXECUTED seam.

**⛔ THE SOUNDNESS CONDITION IS ABOUT THE SEAM, NOT THE INTERVALS.**
The REGAUDIT ceiling used "edge-closed intervals"; that is NOT
executable - a seam inside a loop body re-runs per iteration and its
reload reads a frame the register has been updating. The executable
condition: NO branch edge crosses the seam, making it a LINEARIZATION
POINT - with ONE refinement that turned reach from zero to real: an
edge TARGETING the seam pc exactly is legal, side-patched by the
fixup's own emission position (a source before the seam lands on the
PRE-seam label and runs the eviction; a source after lands past it and
cannot re-run it). That is the loop-exit-lands-on-the-next-init shape,
which is precisely where sequential-loop sharing lives. The seam is
emitted BEFORE the pc's label for the same reason.

**Interpreted excursions**: the entry stubs install the cache state
AS OF THEIR PC (a base-state snapshot plus every seam at or before
it); exits intern per-state as always, so post-seam exits flush the
new occupant. Argfuse treats every seam-installed slot as pinned (its
frame is stale inside its range). The rax coverage gate ran earlier
and is CONSERVATIVE about chained slots (rax and sharing do not
coexist); the gate's popped 13th pick now becomes a spill home
instead of being dropped (a hole inc-2 exposed: dropping it starved
the plan of early-ending pins AND wasted a ranked slot).

**Measured (callgrind, OPT=1 ASSERTS=0, vs increment 1):** the reach
census finds THREE corpus programs executing seams (68_nested,
15_elem_rmw_arith, 17_elem2_divmod_roles); 68_nested reads +0.36% Ir
- the seam's evict+install pair on a shape where the shared
register's saved slot traffic does not recoup it. Everything else is
byte-identical (no plan, no cost). The placement COST MODEL - decide
pin vs home vs share by weight, and decline a share whose seam
outweighs its uses - is the named follow-up, not a patch-over.

**OPEN watched-check, stated honestly:** disabling the stubs'
seam application stays green in every shape built so far - the raise-
resume stub demonstrably EXECUTES (g_jit_entry_resume) in the engage
case, yet the wrong base state never propagates to an observable
value there. The application code stays (it is reasoned-correct and
cheap); a shape that makes its absence bite is still owed. The other
pieces are watched: the two-phase decline case's first spelling was
itself caught legitimately chaining backward, and the fully-
overlapping spelling pins the decline.

## #96 THE PLACEMENT COST MODEL (2026-08-22) - and what it honestly is

**The rule that landed:** a SEAM is taken only when its alternative is
not free - overflow that fits the home tier keeps the home (0 measured
cost), and only the residue past MAX_SPILL_HOMES chains. The capacity
is applied AFTER the share plan, so the plan finally sees the true
tail (the old order truncated it first, which made the gate vacuously
"always home"). MYLANG_JIT_FORCE=rshare ignores the cost half and
chains everything chainable - the FORCE contract - and the -rt engage
sweep runs under it via `g_jit_force_extra` (the in-process force
override, g_jit_xrot's pattern, with `jit_lever_bit(name)` so tests
never touch the enum). WATCHED both ways: the unforced engage shape
must execute ZERO seams (a seam with a free home available is the
defect class), and the forced sweep still proves the machinery.

**What the model deliberately does NOT gate, with the numbers:**
 - PINS: the MAXPINS sweep's flat marginals stand; wall-relevant via
   dependency chains, never measured negative.
 - HOMES: measured a WIN where reach exists - 68_nested reads -2.1%
   Ir homes-on vs MAXSPILL=0 in the same binary. No gate.

**The attribution lesson, earned twice in one arc:** inc-2's
"68_nested +0.36%" was attributed to the seam; with the seam
cost-declined the +0.36% REMAINED (it is placement/layout drift from
the coverage-pop-to-home change - the pop-insert shifts every home
index), and turning homes OFF to "recover" it read +2.5%. Callgrind
attribution is not mechanism (the memory rule by that name), and a
cost model must be anchored on a mechanism A/B (the MAXSPILL /
MAXSHARE / FORCE levers exist precisely for that), never on a
single bench's cur/base residue.

## #96 TWO-ADDRESS SHIFTS + THE LAST rax WIDENING (2026-08-21)

**What.** The in-place literal-count shift - `s >>= 1` / `s <<= 1`
(and their spelled-out `s = s >> 1` twins, which lower identically) -
on a PINNED destination emits the two-address form `sar/shl pin, imm8`
with NO rax staging, exactly like the arithmetic two-address family.
Selection: IntShlRI/IntShrRI with `target == a_slot`, a literal count,
no forwarding pairing, dst pinned (`creg >= 0`). Semantics preserved
in the emit itself: a count >= 64 saturates (`shl` -> `zero_reg32`,
`sar` -> imm 63), a count of 0 emits nothing, and a negative literal
count never reaches here (`imm_shift_ok` keeps the op interpreted).
Counter: `g_jit_two_addr_reg` (shared with the arith family).

**And the rax whitelist widened to match** (`run_may_pin_rax`, since DELETED - endgame Phase A): the
in-place literal IntShlRI/IntShrRI form no longer stages through rax,
so it JOINS the accumulator-shape whitelist and a 13-slot kernel
containing such shifts still pins rax. The coverage gate lists the two
opcodes in its pinned-target case. WATCHED both ways in
`jit_rax_pin_test`: the admission case (arena-conditional want, since
the off-arena budget is 12) and the DISTINGUISHING decline - an
immediate-count shift that is NOT in-place still stages `sar rax, imm`
in every form, and with the shape gate disabled that case aborts
("write to a PINNED register") while the budget arithmetic saves the
variable-count case.

**Measured (RULE B1, OPT=1 ASSERTS=0, interleaved --baseline):**
cur/base geomean 1.007x over 85 benches on a box the calibration
marker flagged ~45% slow - i.e. flat within the machine's noise; the
regs family reads 0.98-1.04x. The win is shape, not a headline: probe
kernels drop the `mov rax, slot; sar rax, imm; mov slot, rax` triple
to one instruction (611 two-address emits in the probe, `sar r14, 1`
in the dump), and rax stays pinnable in shift-bearing kernels.

## THE TYPED FLAT SHIFT/BITWISE COMPOUND STORE (2026-08-21)

**What.** `a[i] <<= n` (and `>>=` `>>>=` `&=` `|=` `^=`) on a proven flat
int array now lowers to **StoreElemInt** with the base op in `aop` -
before this it fell to the boxed StoreElemValue (correct, boxed). The
codegen's StoreElemInt map asks `compound_assign_base` (the single
table); `vm_store_elem_int_body` dispatches the six new bases, the shift
arms checking the negative count BEFORE the COW clone via the
loc-stamped `vm_store_throw_negshift` (the div0 pattern - a throwing
store must not detach slices, intptr-observable, and the tree-walker's
flat_store_core already throws before its clone). Caret parity for the
uncaught negative-count element shift is byte-identical across all
three engines (verified) and the COW non-detach agrees engine-wise.

**The JIT split:** the BITWISE three inline like plus - op_rr2
already encodes and/or/xor, no new guards (non-throwing, bool storage
compile-unreachable) - in BOTH the hoisted (C1e) and ordinary inline
arms. A LITERAL-count shift is the imm8 arm: it cannot throw, and
saturation is an EMIT-time decision (c >= 64 -> zero_reg32 / sar 63,
c == 0 emits nothing - the two-address IntShlRI treatment). A
REG-count shift inlines too (the third increment, 2026-08-21): its
negative-count test is a runtime js DECLINE to the helper, emitted
BEFORE the COW prep (a throwing store must not clone; the helper owns
the loc-stamped throw), and the shift itself is `shl/sar/shr rax, cl`
inside a BRACKETED rcx borrow - push rcx / mov the count in / shift /
pop - because rcx may be a PIN or even one of this very tier's own
scratch registers (sc.idx / sc.data): the window is exactly the
shift, straight-line, between load_elem_q and store_elem_q, so a
clobbered-and-restored rcx is never read inside it and no decline
edge crosses a push (the release-only stack-skew class). The runtime
>= 64 saturation branch is internal to the window. Only a NEGATIVE
LITERAL still declines at emit time - it always throws, so the
helper is the whole story.

**Proof (three counters, because a value oracle cannot tell the flat
helper from the boxed fallback):** the tier test's bitwise case pins
`fast_exact = 56` (32 fills + 24 bitwise, ALL inline); the
literal-shift case pins `fast_exact = 72` (32 + 8*5, covering normal
counts, the c >= 64 saturation and c == 0, values reseeded per
iteration so the zeroing cannot make them vacuous);
`jit_store_elem_shift_regcount` (argument runtime()-wrapped because a
literal arg SPECIALIZES the function and folds the counts to
literals - the vacuous-test trap's shape-eater #6, watched doing
exactly that) pins TWO episodes: the hot reg-count loop at
g_jit_store_fast == 56 with ZERO flat-helper runs (all inline), and a
runtime negative count whose js decline routes exactly the offending
store to the helper (delta >= 1) and surfaces the InvalidValueEx - an
inverted polarity would compute cl&63 garbage inline and throw
nothing. The helpers bump `g_jit_op_run[StoreElemInt/Float]` (they
did not before), which is what makes helper-vs-inline attribution
assertable at all.
WATCHED: reverting the codegen admission leaves every value right and
fails the helper-count fact; dropping bitwise from the inline
admission fails fast_exact 56; dropping the literal-shift admission
fails fast_exact 72; re-declining the reg-count form fails episode 1
(fast 32, helper 24) - each sabotage run separately, committed tree.

**Measured (RULE B1: build/ deleted, --mylang build-claude/perf +
--baseline a worktree build of the language commit, header read, both
OPT=1 ASSERTS=0, interleaved):** 87_elem_shift_compound **0.06x
cur/base (0.060s -> 0.004s)** - the baseline runs all six compounds
boxed (StoreElemValue -> slot_rmw), HEAD runs them all in the inline
tier; 86_elem_arith_compound 0.97x (flat, as expected - the arith
compound tier predates the baseline). my/python: 86 at 0.04x, 87 at
0.03x. Both benches read ~5ms at scale 1 on this box - candidates for
the scales.txt tuning pass.

**Reg-count arm measured (callgrind Ir, OPT=1 ASSERTS=0 both sides,
a 6M-reg-count-shift probe, deterministic):** 918.98M -> 125.00M Ir,
**-86.4%** - six million helper calls become six million inline RMWs.
The bench (87) uses literal counts and is untouched by this arm.

**Bench reach (new, 2026-08-21):** NO bench contained ANY compound
element store - not even `a[i] +=` - so the whole #92/#95 compound arm
had zero bench reach. `86_elem_arith_compound` (+= -= *= /= %=, values
kept non-negative so truncating and flooring division agree with the
Python twin), `87_elem_shift_compound` (<<= >>= >>>= &= |= ^= with
literal counts, values masked below 2^60 so `>>>` equals Python's `>>`
and `<<` never wraps) and `88_elem_float_compound` (the FLOAT twin:
f[j] += *= -= /= %=, a contracting recurrence with a time-varying add
keeping every value in [0,1) so the three languages' float `%` sign
semantics agree) close it, with .py and .cpp twins.

**THE FLOAT TWIN OF THE SHIFT/BITWISE ARMS IS VACUOUS BY
CONSTRUCTION** - shifts and bitwise ops are int-only, so a proven
StoreElemFloat can never carry one (`f[0] <<= 1` / `f[0] &= 3` are
compile errors) - and that claim is MACHINE-CHECKED: two -rt pins fail
the day such a compound becomes legal, at which point StoreElemFloat
needs the arms. The float element tier's ONE remaining compound
residue is `%=` (the fmod helper, pinned by a value test): inlining it
means a libm call inside the RMW tail, which needs the full call
prologue/epilogue - at that point it IS the helper, minus only the
LValue* formation. Declined as not worth a call-bearing tail; revisit
only with evidence of a hot fmod-store loop.

## #96 RE-OPENED: THE STAGING-CLOBBER BUG + THE FIRST MANDATE
## CONVERSION (2026-08-22)

**The mandate (maintainer):** an emitter site may demand a specific
register only with a HARD reason - a calling convention, an
instruction that requires it. Everything else MUST go through
alloc_scratch(caps). regcensus.py enforces it: sites are
bracketed / tagged (reg:isa, reg:abi, reg:conv - validated, stale
tags reported) / UNJUSTIFIED, and `--gate` ratchets every register's
UNJUSTIFIED count against scripts/regcensus_floor.txt in both
directions, run by driver_checks.sh in every lane.

**THE SITE AUDIT'S FIRST READ FOUND A SHIPPED WRONG ANSWER (the
default configuration).** The flat compound store's decline path
staged the INDEX into rsi, then read the rhs "cache-aware" from the
rhs slot's pin - which WAS rsi in a >= 6-pin run - so the helper
divided by the index: `a[j&7] /= d` with d == 0 COMPLETED (dividing
by 7) while tw / -nj / OFF=all all threw DivisionByZeroEx.
Byte-proven in -vdj hex: `mov rsi,[rbx+0x180]` (idx) then
`mov rdx, rsi` (the "cache-aware" rhs read). The prologue-first
note's premise - "a spill does not INVALIDATE ... only the `call`
clobbers" - missed that THE ARGUMENT STAGING ITSELF clobbers pin
registers that later cache-aware argument reads still trust. The
same class silently corrupted the stored VALUE for negative-index
compound stores (the helper wraps the index and uses rhs = idx),
and the LoadElem2 helper tail staged the inner index after the
outer with the same hole.

**The fix - read_slot_avoid / load_operand_avoid:** between
emit_call_prologue and the call, every cache-aware argument load
AFTER the first passes the mask of already-staged targets; a pin in
the mask reads its FRAME SLOT instead, which the prologue's spill
made current. ONLY sound in that window (elsewhere a pinned slot's
frame copy is stale - read_slot's contract, restated at the seam).

**The first mandate conversion, forced by the same audit:** ord()'s
INLINE arm wrote literal RDX (index) and RCX (data pointer) with NO
prologue to spill a pin - a pin in either register meant a tracker
abort in debug and silent corruption in release, reachable the day
a pinned-enough run contains OrdCharV. Both registers now come from
`alloc_scratch(CAP_MEM_BASE, prefer)` - prefer keeps the legacy
bytes while the register is free (the -8 discount always beats an
unpreferred caller-saved weight), and a pinned register simply
yields another. Allocation failure skips the inline arm (the helper
serves). The hand-coded `0F B6 04 11` became the generic
`movzx_byte_bi` (REX only when an extension bit is needed, so the
legacy spelling is byte-identical) - RAWENC 20 -> 19.

**The net:** `jit_staging_clobber_sweep` - four programs x all nine
pool rotations, each checked against the tree-walker oracle (or a
literal want): the divide-by-the-index throw, the negative-index
silent value corruption, the elem2 inner-index helper shape, and
the ord() sum. A rotation sweep, because the pin-assignment
coincidence is exactly what a fixed preference order never tests
(the r9 lesson). Census after: UNJUSTIFIED 1137 -> 1132, RAWENC
20 -> 19; allocator-API lines (prefer/exclude masks) are census
INPUT, not bypasses, and are exempted.

## Phase A of the register-allocator endgame (2026-08-22) - the rax
## whitelist and its coverage gate are DELETED; conflict-evict + a
## one-shot re-emission replace them

`run_may_pin_rax` (the hand-audited list of "rax-free" op shapes) and
the pick-time coverage gate (every whitelisted op's target must be
pinned) were the last two audited opcode tables keeping rax out of
the general pin pool. Both are gone. The replacement is structural:

 - the pick hands rax out OPTIMISTICALLY (it is last in preference,
   so only 12+ hot-slot runs reach it);
 - any emission event that cannot coexist with a rax pin calls
   `Emitter::rax_pin_conflict()` - a helper-call bracket (after a
   call the ABI status and the pin COMPETE for rax and no reload
   order reconciles them), an accumulator ask (`acc_take`), or a
   raise-path reuse window. The seam EVICTS the pin from the model
   (cache list + ra bit), flags `e.rax_conflict`, and the DOOMED
   attempt finishes emitting - tracker-consistent after the
   eviction, its runtime wrongness irrelevant because
 - the chunk then RE-EMITS ONCE with `g_jit_rax_denied` set (the
   `retry_emission` label: a backward goto that destroys and
   reconstructs the Emitter and every per-chunk local - the same
   total-discard semantics as the emit_ok=false give-up), and an
   ML_CHECK proves the denied attempt can never conflict again.
 - rax stays out of ORDINARY alloc_scratch grants (excluded like
   callee-saved): the accumulator convention needs rax reachable
   only through acc_take and the pick. Found the hard way on the
   first -rt: a table-register grant took rax and the next
   accumulator ask found its register occupied.

MEASURED EQUIVALENCE: vdjcmp 116/116 BYTE-IDENTICAL against the
gates. rax pins survive exactly the runs where no conflicting event
fires - the whitelist's semantic set, now derived from what emission
DOES. The cost: one discarded emission per conflicting chunk
(`rax_retries` in MYLANG_JITSTATS; 83_regs_int_40 retries once -
its 40 accumulators exceeded the old coverage too - while
80_regs_int_08 keeps its rax pin with zero retries).

## Endgame B2 (2026-08-22) - the fwd bus DECLARES; the ModRI evict gap

A pin-producing forwarded op no longer pays `mov rax, <pin>` to put
its value on the bus: it sets g_fwd.res_reg = <pin> and the consumer
reads emit_fwd_bump's return. Measured honestly: pure mov MIGRATION
today - every corpus consumer of a forwarded pin value is DESTRUCTIVE
(it computes into the value), so the copy moved from producer to
consumer and no count changed (21 programs drift by position only).
The structural point stands: the bus is DECLARED state, no consumer
may assume rax, and a read-only consumer takes the pin directly the
day one appears. The div consumers (IntModRI/IntAddModRI/IntBin's
arm) copy a non-rax bus value into rax - the ISA's register, paid
only when it differs. Plus the Phase A completeness fix: IntModRI /
IntAddModRI stage in rax raw BY ISA with no ask and no bracket, so
they invoke the conflict eviction themselves - without it a
rax-pinned attempt would have clobbered the pin unretried (a shape no
corpus program has: 12+ hot slots AND a specialized mod - closed on
inspection, not by a failure).

## Endgame B2c (2026-08-22) - the conflict seam generalizes; the rcx
## shift scan is deleted

reg_pin_conflict(r) works for ANY register now (pin_conflicts is a
mask; the re-emission accumulates a denied mask and is bounded by the
pool size). First consumer beyond rax: the RR-shift's raw CL load
evicts a pinned rcx itself, so the per-op rcx scan in jit_xcache_busy
- the table added when the WATCHED 13-pin `sb += sa >> k` shape
clobbered a pinned rcx at every rotation - is DELETED. Same final
emission, structurally derived: vdjcmp 116/116, the xrot matrix
green. run_may_pin_rdx is the last per-register whitelist standing;
its deletion plan (the element-tier role registers + the div arms)
is in plans/register-allocator-endgame.md B2c.

## Endgame B2c-rdx (2026-08-22) - run_may_pin_rdx, the LAST
## per-register whitelist, is DELETED

rdx is optimistic now. Every raw claimant participates: the div arms
and both store tiers call the conflict seam at entry; cqo/idiv_reg/
imul_reg are SELF-DECLARING (the encoder calls reg_pin_conflict at
the one place the ISA claim is made, covering every present and
future emitter); the ctx-chain table fallback evicts (its "refusal
implies no pin" argument broke under all-pinned pressure); and every
ask whose value must survive cqo/idiv/imul EXCLUDES rdx (hold gained
an exclude parameter; div_magic's keep - which DIED at the imul two
instructions after a grant returned rdx, a wrong `k % 2` watched
live at zero pins - is the case that proved the deny bit had been
doubling as a grant filter, the r8 lesson verbatim). One deliberate
non-conversion: elem_scratch_plan's COUNT stays literal rdx - the
remainder handling depends on count==rdx post-idiv, and picking it
also ate a reservation candidate (two watched value failures); the
tiers claim rdx at entry instead.

MEASURED: 88/116 byte-identical; 27 programs drift by pure grant
substitution (ties that broke to rsi now break to rdx - counts equal
program by program); 43_sieve drops SIX instructions (rdx pinning
gained it a register). -rt 1946/1946 both arenas; corpus + xrot
green; gate at zero floors.

## Endgame B1 (2026-08-22) - the type-singleton holders become
## MODEL-OWNED, RUN-SCOPED GRANTS; jit_xcache_busy is deleted

The last static pool exclusions - rsi always, r8 via the
run_needs_float_tag opcode scan, both only off-arena where a Type
tag does not encode as an imm32 - stopped being a convention
re-derived at every consumer and became ONE recorded decision:
`Emitter::grant_tag_regs(float_live)` runs once per run BEFORE the
budget and the pick, records the holders (tag_int_reg/tag_float_reg,
still the conventional rsi/r8 for byte identity - Phase D assigns
them like any interval) plus a `tag_granted` mask, and the mask is
claimed as **ra.busy** - the reservation IS the allocator's
occupancy, so the pick, the element-tier reservation, the pool
budget and every scratch ask all refuse the holders from the same
fact. `check_pins_are_busy()` now ML_CHECKs the grant survived every
allocator reset, at every allocation seam.

THE SEAM HARDENED WITH IT: store_type_tag / cmp_reg_tag /
cmp_rax_tag LOST their caller-supplied holder parameter - the
emitter looks the holder up via `tag_holder(tag)`, which ML_CHECKs
the grant was actually taken. The old parameter was a promise a
caller could break silently, and one DID: store_dst_bool passed a
register that held no tag and shipped a wrong answer (the
store_type_tag_via record). That bug class is an emit-time abort
now. ~25 call sites dropped their literal RSI/8 argument; the
write-only float_tag_live field is deleted (the grant carries the
decision); elem_reg_usable_nopin consults the grant mask instead of
re-deriving the imm tests; run_needs_float_tag survives solely as
the grant's input (still fail-safe: an unknown op keeps the tag).

TWO LESSONS PRESERVED from the deleted jit_xcache_busy, both blocks
against naive pool widening: r8 is ALSO SysV arg 5 + raw scratch
inside call brackets - freeing it from the TAG was necessary, never
sufficient (four float tests failed when tried early); and rcx is
ISA-fixed by the variable-count shifts - the RR shift emitters evict
a pinned rcx through the conflict seam (WATCHED 2026-08-20: a
runtime shift count clobbered a pinned rcx at every rotation until
the tracker named it).

AND A LATENT HAZARD CLOSED: the cache-barrier path did
`clear_cache_state(); e.ra = RegAlloc()` - the second reset wiped
`denied` (and would have wiped the grant), so ONLY the barrier'd
op's emission saw an empty denied set: a scratch ask inside it could
take a register the whole run was told not to spend (a live hoist's
r10, a withheld reservation). The redundant reset is deleted;
clear_cache_state keeps `denied` per its own documented contract and
now keeps the grant too (`ra.busy = tag_granted`).

MEASURED: 115/116 byte-identical ON AND OFF the arena - the
conversion itself is exact. The single drift is the barrier fix, not
the grants: 73_multi_unpack's raise-path loc stamp after a barrier'd
MultiUnpackV now draws its scratch with the run's denied set visible
(rcx withheld there), landing on r10 instead of rcx - 4 instructions,
pure substitution, counts equal. -rt 1946/1946 x both arenas x
gcc+clang; TESTS=1 OPT=1 ASSERTS=0 -rt green both arenas; corpus
plain/nolowmem/xrot(both arenas) green; census gate at zero floors -
where it also caught cmp_rax_tag's callers losing their justifying
tags (the register lives in the METHOD NAME, the sixth audit-table
shape, seen by the accessor-derived scan).

## Endgame B3 (2026-08-22) - the hoist pair is claimed THROUGH THE
## MODEL at region entry; the hand-built clobber-mask entry is deleted

`if (has_hoist) clob |= HOIST_REGS_MASK` - the run-wide, pick-time
deny of r10/r11 - is gone. The REGION claims its pair at its own
entry: a pin holding r10/r11 (the pick is optimistic about them now)
is a conflicting event - evicted via the seam, the retry denies it,
and the second pass arrives at the preheader with both registers
free; the claim is then ra.busy, so the elem plans, every scratch ask
and the barrier machinery refuse the pair from the same fact the tag
holders use. The claim is held to run end, matching the deleted
mask's deny scope for scratch (region-scoped release is a Phase D
refinement); elem_scratch_reserve still models the future claim at
pick time via has_hoist.

GENERALIZED WITH IT: `Emitter::claim_mask` - every run-scoped,
non-pin claim (the B1 tag holders, the B3 pair) in one mask, restored
by clear_cache_state (a barrier no longer wipes a live claim) and
enforced by check_pins_are_busy at every allocation seam. A new claim
class ORs itself in.

REACH, proven not assumed: NO corpus program pins into r10/r11 in a
hoist run (their hot sets fit the callee-saved budget), so the
eviction path would have shipped untested - the `jit: B3` -rt case
constructs the shape (seven hot int accumulators + an array walk),
asserts values, region entry AND that the retry actually fired, and
was WATCHED FAILING: with the eviction removed the tracker aborts by
name ("write to a PINNED register: r10", the region's nav()).

MEASURED: 104/116 byte-identical, both arenas. The 12 drifting
programs are all hoist runs and every one is a PURE substitution
(instruction counts equal program by program): transient scratch asks
before/after the region now win the freed r10 (weight 3) over
rsi/rdx (weight 6 - the SysV-arg penalty), which is the cost model
applied to a candidate set the hand mask had been truncating.
-rt 1947/1947 both arenas; corpus plain/nolowmem/xrot green; census
gate at zero floors; TESTS=1 OPT=1 ASSERTS=0 and clang lto0 green.

## Endgame C2 (2026-08-22) - the model learns the xmm FILE; the float
## tracker exists

The float side had NO model at all: pins assigned by a positional zip
(`FCACHE_REGS[h]`), no occupancy record, no tracker - an xmm clobber
was a silent wrong float. Now:

 - `fp_allocatable/fp_weight` (the caps model): xmm2..7 allocatable -
   xmm0/xmm1 are the per-op float scratch convention (the float
   side's rax; helper results arrive in xmm0 per SysV) reachable only
   through their conventional sites until C3 converts them, and
   xmm8-15 need a REX the float encoders do not emit (an encoding
   CAPABILITY fact; they join when the encoders do). All xmm are
   caller-saved, so no unsaved-callee-saved hazard exists;
 - `RegAlloc.fbusy` + ftake/ftake_fixed/fgive, and the Emitter seams
   `alloc_fscratch(prefer, exclude)` / `free_fscratch` / `ftake_reg`;
   the C2a pin assignment goes through the register STATE (first-free
   over the same pool = byte-identical, the N5 argument);
   check_pins_are_busy verifies fcache against fbusy at every
   allocation seam; clear_cache_state drops the float occupancy with
   the pins (no float claims exist yet);
 - `fwrote(x)` - the float tracker, in every xmm-WRITING encoder
   (fload, farith, fmov_rr, sqrtsd, movq_xmm_from, cvt, load_elem_sd,
   cvtsi2sd_elem, farith_x1_x0, pxor_x1; the store forms READ their
   xmm and are not hooked). Gates mirror wrote(): machinery, flushed,
   call bracket; no borrow arm (push/pop do not cover xmm). The ONE
   legitimate mid-op pin write - write_fslot's cached arm - declares
   itself with PinMach exactly like write_slot's GP twin, and the
   entry loads sit in a PinMach block like the GP entry loads.

WATCHED FAILING: removing the write_fslot declaration aborts -rt by
name ("write to a float-PINNED xmm register with no declaration: r5",
opcode 54) - the net C3's 89-site conversion needs, proven before the
conversion starts. MEASURED: 116/116 byte-identical, both arenas
(vdjcmp vs HEAD); -rt 1947/1947 both arenas; relna + clang lto0
green; census gate at its floors.

## Endgame C3 batch 2 (2026-08-22) - the float staging pair is a
## RUN-SCOPED GRANT; the xmm census reaches ZERO

The 81 remaining hardcoded X0/X1 sites were one seam, not 81 edits:
the per-op float staging pair became a run-scoped grant
(`Emitter::grant_fstage`, the B1 tag-holder pattern - prefer
xmm0/xmm1 on the fresh allocator, so it always lands there and the
conversion is byte-identical by construction), and every staging
site reads `fsa()/fsb()` instead of naming a register. Holding the
grant run-long is ALSO what makes the float fwd bus sound with no
per-op exclude dance: a forwarded value sleeps in a granted
register between ops, and nothing else can be granted it.

With it: fp_allocatable includes xmm0/1 now; the fixed-pair encoders
`farith_x1_x0`/`pxor_x1` are DELETED (the sixth audit-table shape's
rule - generic farith/pxor_rr replace them, byte-for-byte identical
encodings); the element tiers' BARE-NUMBER xmm operands (0/1 as a
literal argument - the census's blind spot) converted to queries
too; and the fmod arm ML_CHECKs that the stage IS the SysV pair,
because force_x0_x1 conflates "force the stage" with "libm wants
xmm0/xmm1" - sound while they coincide, and a Phase D stage move
must add marshalling moves there instead (the check names it).

What stays literal is the SysV float ABI alone: 8 XMM0 + 1 XMM1
sites, all abi-tagged (libm args/returns, jit_put_float's xmm0,
the movx0 counter's comparison).

THE FLOAT CENSUS IS AT ZERO - same day it was first measured (61+28
this morning). Floors: XMM0 0, XMM1 0, TOTAL 0. MEASURED: 116/116
byte-identical both arenas; -rt 1947/1947 both arenas; corpus both
arenas + driver green; relna + clang lto0 green. Phase C of
plans/register-allocator-endgame.md is COMPLETE (xmm8-15 join when
the float encoders learn REX - a capability gap, recorded there).

## Endgame D0+D1 (2026-08-22) - the Ir baseline ledger; live
## intervals with holes

D0: callgrind Ir over the pressure corpus (the 80..85 regs family +
float/hoist/loop anchors) recorded in the plan at sha 039a30c - the
cross-session-comparable anchor for Phase D's ending A/B (the wall
half must be interleaved against a binary rebuilt from that sha).

D1: `jit_build_intervals` - the allocator's input representation.
Per-slot MAXIMAL live stretches with holes ({slot, [start,end),
weight}), built from the jit_slot_liveness fixpoint + jit_op_slot_refs
under the spec "an interval covers pc iff live_in(pc,s) or s is
defined at pc" (a def opens the stretch at the defining pc; barrier
ops read as use-everything/define-nothing). The `jit: D1` -rt check
is derived from that SPEC, not from the builder - coverage both ways
at every (pc, slot), per-slot disjointness, weights recounted - with
a HOLE vacuity guard: some slot in the corpus must produce two or
more intervals, or the representation's whole point (one variable,
several stretches, several registers) went untested. WATCHED
FAILING: removing the def-opens rule fails with mismatches named by
pc and slot. Pure analysis - no emission change (vdjcmp 116/116,
gate, both arenas, clang lto0 all green).

## Endgame D2 + validator arm 1 (2026-08-23) - the per-pc seam;
## slot_mem_check

D2: reg_at/spill_at/freg_at - "where does slot s live AT cur_pc?" -
the one question every emitting consumer asks now (37 sites
migrated; dbg_pc promoted to cur_pc, model state). Byte-identical
by construction; D3 swaps the wrapper's body for the interval
assignment without touching a consumer. "The tracker learns the
map" deferred to D3 with the reason recorded in the plan (the
wrapper checked against itself is the oracle-shares-its-subject
trap).

VALIDATOR ARM 1 (the D4 oracle, built BEFORE the brain it guards):
`slot_mem_check` - every [rbx+disp] PAYLOAD access through the slot
accessors (load/store/fload/fstore/cvt) aborts when the assignment
at cur_pc homes the slot in a register or a spill: the frame word
is STALE and reading (or writing under) it is the silent-wrong-
answer class D3's moving homes would otherwise create. Payload-only
on purpose (the TYPE word legitimately stays memory-resident for a
pinned slot); gates mirror wrote() (machinery / flushed / call
bracket). ITS FIRST RUN FOUND A FINDING: emit_call_prologue's pin
SPILLS were undeclared machinery - the save half of the
spill-around-a-call discipline never said so, while the epilogue's
reload half always did (PinMach). Declared now, at the source.
WATCHED FAILING: a read_slot that bypasses the seam (the exact
D3-era consumer bug) aborts by name with the slot number.

Emission untouched: vdjcmp 116/116 both arenas; -rt 1948/1948 both
arenas; corpus + xrot-off-arena + gate + driver + clang lto0 green.

## Endgame D3.b step 1 (2026-08-23) - pick_visit_op, the shared
## op-classification visitor

WHAT: pick_cached_slots' 537-line qualification switch - the rules
that decide, per op, which frame slots are touched and through which
choke point - is now `pick_visit_op(ck, in, pc, v)` (jit.cpp, directly
above the pick): a template over a visitor struct with ten callbacks
(usei / usei_dst / usef / bad / badi / badf / use_ret / fdst_mark /
full_read_mark / mark_barrier; the contract is the function's header
comment). The pick binds its accounting lambdas via a local Fns struct
of lambda references; `return false` means "unclassifiable, or written
slots not enumerable per-pc" and the pick answers by caching nothing -
the exact old `return {}` semantics, including ForeachDynNext's
in-case early return.

WHY: D3's interval allocator must qualify interval-local uses by the
SAME rules the pick has always applied. Two copies of 537 lines of
qualification would drift silently, and a dropped bad() in one of them
is a wrongly-pinned slot - the r9 shape. One switch, two drivers.

HOW VERIFIED (pure code motion): the extraction was comment-aware
(a /* */ state machine; only code segments rewritten) and
count-asserted per callback (160 call sites: usei 28, bad 82, badi 14,
usef 14, mark_barrier 7, usei_dst 6, fdst 5, badf 2, full_read 1,
use_ret 1 - the script aborts before writing on any drift). Oracle:
vdjcmp 116/116 identical in BOTH arenas (lowmem + MYLANG_NO_LOWMEM),
-rt 1690 x 5 modes x both arenas, corpus_diff 25/25, driver_checks,
the census gate at floor, TESTS=1 OPT=1 -rt, and the clang OPT=1
ASSERTS=0 LTO=0 lane with zero warnings (the CI shape that caught the
retry-bound regression).

NOTE the gcc trap: a local struct member named `usei` cannot be
declared `const decltype(usei) &usei;` - "changes meaning of usei"
(-fpermissive). The lambda types are `using`-aliased first.

## Endgame D3.b step 2a (2026-08-23) - per-interval qualification

WHAT: `jit_qualify_intervals(ck, begin, end, iv, out, &orphans)`
(jit.cpp, declared in jit.h with the IntervalQual struct) - the first
CONSUMER pick_visit_op was extracted for. It drives the shared
classification switch with the interval side's callbacks: each event
(usei / usef / bad / ...) lands on the LiveInterval covering
(slot, pc), so an interval is judged by the uses inside IT, not by the
whole run. Raw facts per interval - uses_int / uses_ret / uses_float
counts, wrote_int / wrote_float / full_read / mem_int / mem_float
flags; the pool derivations (GP / XMM candidacy, C3 elidability) are
stated in jit.h and left to the scan. `orphans` counts events no
interval covers - a live drift detector between visit_use_def (the
liveness the intervals derive from) and pick_visit_op (the
classification), since the two tables here run at the SAME stage.

WHY: the endgame plan's payoff shape - one late boxed op (a DictStore
key) no longer costs a slot its whole run; the slot's earlier hot
interval stays register-eligible. This is the fact base the D3 scan
allocates from.

NET: the `jit: D3.b` -rt check pins the AGGREGATION against the pick's
public answer (via a TESTS-only export of the static pick):
 A. every picked slot is per-interval clean (weight >= 3, no mem_int,
    no float facts); A-float likewise for fhot (wrote_float, >= 3
    float uses, ZERO countable int uses, no mem_float);
 D. every local refused despite >= 3 pure-int uses carries mem_int on
    some interval (max_pins generous, so disqualification is the only
    refusal left);
 B. the payoff shape observed + vacuity-guarded: a run-refused slot
    owns a clean >= 3-use interval;
 C. orphans == 0.
WATCHED FAILING both ways: bad() stripped of mem_int -> property D
names all four slots; attribution disabled -> property C (22/29
orphans) AND property A (picked slots with zero recorded weight).

[CLOSED 2026-08-23: the float-return exemption case landed - the
qual check iterates FUNCTION chunks and a returned float accumulator
pins it: an fhot slot the ReturnV reads proves use_ret disqualified
nothing, vacuity-guarded (saw_ret_exempt), WATCHED failing by
sabotaging the shared switch's ReturnV case to usei() - which kills
the pick's own fhot through the same switch, exactly the
one-switch-two-drivers design detecting its own corruption. Still
recorded: the corpus-wide orphan census (rides with the scan).]

ADDENDUM (same day): the qualifier also emits the MemEvent stream
(optional out) - one {pc, slot, gp_only} per bad()/badi() event, the
FORCED-INTERVAL-END positions the scan cuts on (the per-interval
booleans say WHETHER, the events say WHERE). Property E in the -rt
check requires the stream and the flags to agree in both directions
(every mem_int interval contains a matching event; every event lands
inside an interval whose flags it set), with per-kind vacuity guards.
WATCHED failing: stamping pc+1 fails E in both directions at four
named sites.

Analysis-only: no emission change (vdjcmp trivially identical); -rt
1690 x 5 x both arenas, corpus_diff, census gate, TESTS=1 OPT=1,
clang OPT=1 ASSERTS=0 LTO=0 zero warnings, and the non-JIT compile
check (jit.h's platform test forced to 0, g++ TESTS=1 build + -rt
green) - the new jit.h decls are unguarded, so the check mattered.

## Endgame D3.b step 2b-i (2026-08-23) - the linear scan, as analysis

WHAT: `jit_lsra_assign(ck, begin, end, iv, q, mem, K, out)` (jit.cpp,
contract in jit.h) - the allocator's BRAIN, landed with no lever and
no emission change: the output is a plan (LsraPiece: interval piece ->
abstract register 0..K-1 or memory) and the -rt net checks its
invariants. Three stages: CUT every D1 interval at its slot's MemEvent
pcs (the forced-interval-end decision - the event pc becomes a one-pc
forced-memory piece), admitting a piece only when its interval is
float-free and jit_next_use proves a use inside it; WALK pieces by
start with expire-and-free (lowest free index, deterministic); at
pressure EVICT the active piece whose next use at the contested pc is
furthest, SPLITTING it there - it keeps its register up to the
contested pc, the remainder becomes a memory piece. The split is what
lets one variable use different registers - or none - in different
parts of one run, the maintainer's 10000-line-function requirement.
v1 decisions recorded: no re-queue of the split remainder (the
second-chance re-entry is a quality follow-up), registers are abstract
indices (the physical binding - pool order, callee/caller cost, rax's
ISA facts - is 2b-ii's cost model), GP only (the float twin is a
sibling case for 2b-ii).

NET (`jit: D3.b ... step 2b-i`): I1 TILING (every interval covered
exactly by its pieces - a gap is a pc where the D2 seam's question has
no answer), I2 NO CONFLICT (no two resident pieces share a register at
any pc), I3 FORCED MEMORY (an event pc's covering piece is memory; a
float-fact interval is memory throughout), plus the shape assertions:
the 2a payoff slot's pre-DictStore piece IS resident at K=4 though the
pick refuses the slot run-wide (the payoff, now collected in the
plan), every pick-picked slot resident at K=4, and at K=1 the idle
slot's residency length loses to the best hot slot's.

WATCHED FAILING three ways: evict-NEAREST (comparison flipped) fails
the K=1 comparative property (z resident 9 pcs vs hot 0) AND the K=4
residency; a register handed out without marking it busy fails I2 at
four named overlaps; cutting disabled fails I3 at five named event
pcs.

A FINDING ON THE FIRST RUN, recorded because the correction is a
design fact: "every pick-picked slot is register-resident" is
UNSATISFIABLE at K=1 - three hot slots (i, a, n) overlap in the loop,
so the guarantee evict-furthest actually makes is COMPARATIVE (the
idle slot loses to a hot one), and the K=1 assertion now states
exactly that. Properties of an allocator under pressure are about who
LOSES, not who wins.

Verified: -rt 1690 x 5 x both arenas, TESTS=1 OPT=1, clang OPT=1
ASSERTS=0 LTO=0 zero warnings, non-JIT compile check, census gate at
floor. Analysis-only - no emission change.

## Endgame D3.b step 2b-ii opening (2026-08-23) - the lsra lever
## bridge, and the ReturnV type-evidence rule

WHAT: `MYLANG_JIT_LSRA=1` (g_jit_lsra, default OFF, test-settable) -
the scan chooses the PIN SET: intervals + qualification + MemEvents +
jit_lsra_assign per run, the plan reduced to WHOLE-RUN residency (a
slot every piece of which is register-resident), fed to the existing
machinery in the pick's own order (weight desc, slot asc); any stage
declining falls back to the pick. Execution proof: g_jit_lsra_pins
(JITSTATS row), bumped by emitted code at the entry of a fragment
whose pins came from the scan.

⛔ THE FINDING THE LEVER'S FIRST SWEEP PAID FOR - THE ReturnV
TYPE-EVIDENCE RULE. The first lever-on -rt run failed 11 tests plus a
corpus divergence with a leak: `var f = <closure>; return f;` pinned
f on the strength of its single ReturnV read (my reduction admitted
any weight > 0), and the flush's C3 tag re-establishment then stamped
`mov f.type, <int-tag>` OVER THE CLOSURE's t_func - cf(400) raised
NotCallableEx and the FuncObject leaked. The pick never hits this
because its >= 3 threshold happens to exclude ReturnV-only slots -
its own ReturnV comment says so for floats - i.e. A PROFITABILITY
KNOB WAS SILENTLY CARRYING A SOUNDNESS RULE. The rule is now
explicit at every layer: GP admission requires uses_int > 0 (an
int-op touch, which inference proved - so the t_int stamp is sound);
uses_ret is WEIGHT, never evidence (ReturnV reads ANY type).
 - jit_lsra_assign's cut phase gates admission on it (`evid`);
 - the bridge's reduction gates on wint > 0;
 - property F in the 2b-i net: a piece of a uses_int == 0 &&
   uses_ret > 0 interval is never resident - WATCHED failing (the
   evid gate removed names the slot), on a FUNCTION-chunk case (the
   root ends in Halt, so the shape needs a real callee);
 - the bridge test runs the closure program end to end.

Two coverage tests (jit_xcache_pins, jit_hoist_pair_conflict) pin
g_jit_lsra = false for their duration - they assert the DEFAULT
allocator's mechanism fires on a crafted shape, which the scan's
different pin choice legitimately starves.

Verified: the 2x2 matrix (lever x arena) of -rt 1690 x 5 all green,
corpus_diff plain both lever states + lever-on off-arena, vdjcmp
116/116 BOTH arenas for the default config (everything emission-side
is lever-gated), TESTS=1 OPT=1 -rt in both lever states, clang OPT=1
ASSERTS=0 LTO=0 zero warnings ((void)lsra_chose - the TESTS-only-read
cc1f50f shape, closed preemptively), non-JIT compile check, census
gate.

NEXT (the plan's marker): per-pc emission - serve the D2 seam from
the plan's pieces, entry loads / split stores / label resolution /
exit flushes, validator arms 2/3, then the D0 ledger A/B.

## Endgame D3.b step 2b-iii-a (2026-08-23) - the snap to
## linearization points

WHAT: `jit_lsra_snap` (jit.cpp; contract in jit.h) translates the
scan's plan into the vocabulary the emitter ALREADY EXECUTES - #96
inc-2's seam pattern, generalized: an entry state per abstract
register plus LsraTrans transitions {pc, reg, evict|-1, install|-1},
each sitting on a LINEARIZATION POINT. The rule comes straight from
the share plan's own soundness note, which had already REFUTED
per-edge reasoning about intervals ("a seam inside a loop body re-runs
every iteration"): a register-state transition may only sit at a pc no
branch edge crosses in either direction; an edge TARGETING it is legal
(the seam_pre patching sends pre-transition sources before it and
post-transition sources past it - for an install at a loop head, that
IS the loop-carried pin). A resident piece whose interior start, or
whose CONTINUATION end, cannot sit on such a pc is DEMOTED - never
promoted.

THE CONTINUATION RULE, settled here: a piece end needs a flush exactly
when the interval continues past it (p.end < interval.end - a mem-cut
or an eviction-split remainder reads the slot from memory right
after). A DEATH or HOLE end emits nothing: the register is dropped
silently in the model (stale memory is unread by liveness), and the
drop-at-death discipline guarantees no later flush can clobber the
slot's next definition - which also means installs NEVER evict; every
flush is a continuation-end's own.

The edge scan is ONE exported function (jit_run_edges), now shared by
jit_share_plan, the snap, and the -rt net (which re-derives only the
crossing rule); jit_lin_point is the one crossing test. The
share-plan refactor is byte-identical: vdjcmp 116/116 both arenas.

NET (`jit: D3.b ... 2b-iii-a`): S1 transitions only at linearization
points (crossing rule re-derived from spec), S2 replay => coverage at
every pc (playing entry + transitions must hold exactly the covering
piece's slot), S2b every continuation end OWNS its flush (stated
directly - replay cannot see a dropped flush whose model cleanup
remains: memory state is outside the model), S3 demotion only, S4 the
in-loop-event case demotes (a DictStore in the loop body puts every
boundary on a crossed pc). Vacuity: a mid-run install, a continuation
flush, and a demotion must each occur. WATCHED failing three ways:
demotion disabled -> S1 names the crossing edges; flush dropped with
model cleanup kept -> S2b names the pc; install registered one pc
late -> S2 names three pcs.

Analysis-only (nothing consumes the snap yet). Verified: vdjcmp
116/116 both arenas, -rt default + off-arena + lever, corpus, TESTS=1
OPT=1, clang OPT=1 ASSERTS=0 LTO=0 zero warnings, non-JIT compile
check, census gate.

NEXT: 2b-iii-b - EMISSION: generalize the seam-application loop to
LsraTrans (evict-only and install-only arms), entry stubs + exit
flushes from the replayed per-pc state, behind the lever.

## Endgame D3.b step 2b-iii-b (2026-08-23) - transitions EXECUTE:
## per-pc residency reaches emission

WHAT: under the lever, the snapped plan's transitions are emitted and
run. The seam-application loop gains the generalized arms (an
interior-end FLUSH stores tag + payload and removes the cache entry; a
mid-run INSTALL claims the register and loads - both before label[pc],
the ShareSeam back-edge rule); entry stubs replay entry state + the
transitions at or before their pc; exits and brackets need NOTHING new
- e.cache evolves at the transitions and is truthful at every pc, so
every consumer that iterates it is per-pc automatically. Physical
binding: entry occupants ride the existing take_reg zip (hot is in
abstract-reg order); an areg with only mid-run pieces fixes its
identity (and its callee-saved push) at setup. Execution proof:
g_jit_lsra_trans (JITSTATS), bumped by the emitted transition code -
the phase-handoff shape executes 8 transitions and prints the same
answer as the default engine.

⛔ TWO INTEGRATION DEFECTS THE NETS CAUGHT IN MINUTES, both now rules:

1. BUSY <=> ENTRY IS AN INVARIANT, NOT A CONVENTION. Binding a mid-run
   areg's register busy at setup (no cache entry until its install)
   hit check_pins_are_busy and then AccScratch's "no rax pin present"
   abort - the conflict-evict and accumulator machinery PIVOT on
   cache entries, and a busy-but-empty register is invisible to both.
   Busy is per-pc now: an install take_fixed's its register AT ITS
   PC, a flush gives it back; scratch is op-scoped, so nothing holds
   a register across a transition. (A take_fixed refusal means a
   Phase-A conflict denied the register mid-pass - skip; the pass
   retries with it denied.)

2. TYPE EVIDENCE IS PER PIECE, NOT PER INTERVAL (the d1 finding, a
   shipped-quality wrong answer the lever sweep caught):
   `var dyn d = 5; d = "hi"; d = [1,2]; len(d)` failed - d has ONE
   interval spanning all three defs (boxed redefinitions are liveness
   BARRIERS that glue them together), so interval-level uses_int > 0
   let the post-cut remainder pin an ARRAY as an int and the flush
   stamped t_int over t_arr. The qualifier now emits `int_uses`
   events ({pc, slot} per usei/usei_dst) and the scan admits a piece
   only with an int-op touch INSIDE IT. Property G pins it (a
   resident piece owns an int touch inside itself), watched failing
   by reverting to interval-level evidence - it names the exact d1
   piece [3,4).

MYLANG_LSRADBG=1 dumps per-run intervals + qual facts + pieces +
transitions (the SHAREDBG pattern) - it is how d1 was diagnosed.

Three more coverage tests pin g_jit_lsra = false (jit_range_share,
jit_spill_homes, jit_rax_pin - seams, spill homes and the rax pin are
replaced or absent under transitions by design). The bridge test
gains the d1 program and a transition-counter execution proof.

Verified: the full lever x arena matrix of -rt (1690 x 5 each) and
corpus_diff, PLUS corpus --levers and --xrot COMPOSED with the lever,
vdjcmp 116/116 both arenas for the default config, TESTS=1 OPT=1 both
lever states, clang OPT=1 ASSERTS=0 LTO=0 zero warnings, non-JIT
compile check, census gate.

STILL AHEAD (2b-iii-c+): validator arm 2 as a machine check, the cost
model / physical preference (rax stays excluded from mid-run pieces
via pool order for now), spill homes + C2b hoist regions merged into
trans mode (both currently decline it), the D0 Ir ledger A/B, and the
default flip gated on D4 + the ledger.

## Endgame D3.b 2b-iii-c inc 1 (2026-08-23) - BOUNDARY EXTENSION:
## +14% recovered to +0.2%

WHAT: the snap's blind demotion becomes EXTEND-OR-DEMOTE. A resident
piece whose interior start is not a linearization point slides its
start BACKWARD to the latest legal pc (a lin point, or run begin =
the entry-load set); an interior end slides FORWARD to the earliest
(a lin point, or run end = the exit machinery). The extension covers
only pcs where the slot is untouched (bounded by every other piece of
the same slot) and the register unoccupied (bounded by its reg
neighbours): over such a stretch the register holds a DEAD value -
the pick's own entry-load soundness argument (def-before-use) - and
an extended end's flush writes a dead value, the uniform-flush rule's
price. THE PICK'S WHOLE-RUN PIN IS THE DEGENERATE EXTENSION, so trans
mode can no longer lose to the pick on a shape the pick handled. A
continuation end still demotes off a non-lin pc: its same-slot
successor bounds the extension at zero, and a data-carrying flush
cannot move past its reader.

MEASURED (same-binary env A/B, OPT=1 ASSERTS=0, callgrind Ir,
lever-on vs off): 07_nested_loops +14.29% -> +0.23% - the diagnosed
inner counter (demoted because the outer loop's exit edge crosses its
whole body) now extends to a whole-run-class pin. Still open:
83_regs_int_40 +6.09% unchanged - the COST MODEL item (scan-order
admission + furthest-next-use eviction vs the pick's weight ranking
at 40 slots / 13 pins), next on the 2b-iii-c list.

THE BOUNDS' NET, and an honest record of its shape: both extension
bounds (reg neighbour, same slot) are DEFENSIVE - two crafted cases
and two sabotages could not reach a violation (a lin point nearly
always separates two intervals of one slot, and the eviction-split
truncation shapes refused to line up) - so, per the escape analysis'
reassignment-guard precedent, the enforcement is a MACHINE CHECK
rather than a test: the snap's translation model now refuses BOTH a
register overlap AND a slot resident on two registers (slot_on),
at every lever compile of every program. Property S5 in the -rt net
states the same emission hazard (the second register's flush writes
a STALE copy over the live one), and two new cases pin the extension
shapes that DO occur.

Verified: full lever x arena -rt + corpus, vdjcmp 116/116 default
config, TESTS=1 OPT=1 both lever states, clang OPT=1 ASSERTS=0 LTO=0
zero warnings, census gate.

## Endgame D3.b 2b-iii-c inc 2 (2026-08-23) - the HOME-TIER MERGE:
## 83's +6% was the missing spill homes, not the winner selection

WHAT the diagnosis corrected: inc 2 was scoped as "the cost model"
with 83_regs_int_40's +6.09% as its target - but the emission diff
showed the lever-off fragment serving 13 register pins PLUS 16
native-stack SPILL HOMES ([rbp-...] qwords, one access, no tag,
call-surviving), and trans mode's v1 bound had zeroed the home tier
out entirely. The winner set and the eviction were fine; the
overflow's PLACEMENT was the whole number.

THE MERGE: trans mode now hands its overflow to the existing scache
machinery - a slot with total uses_int >= 3 (the pick's floor: the
home's seed + flush cost), NO mem_int on any interval (a d1-style
boxed redefinition excludes), no float facts, and NO resident piece
(registers or a home, never both) joins spill_hot, weight-ranked.
Carried in lsra_homes and assigned after the split - appending to
`hot` would corrupt the abstract-reg zip, whose order IS the
entry-occupant list (the zip's ML_CHECK caught exactly that draft).
jit_share_plan is DISABLED under trans mode: the transitions own
register sharing, and a ShareSeam chained onto the same registers
would collide with them.

MEASURED (same-binary env A/B, OPT=1 ASSERTS=0, callgrind Ir):
83_regs_int_40 +6.09% -> +0.76%. The six-bench ledger now reads
+0.14%..+0.76% lever-on vs the pick - the headline regressions are
gone; the residual sub-1% (per-call transition code, selection
noise, the textra filter) is cost-model polish, still listed.

Verified: full lever x arena -rt + corpus + --levers composed,
vdjcmp 116/116 default config, TESTS=1 OPT=1 both lever states,
clang OPT=1 ASSERTS=0 LTO=0 zero warnings, census gate.

## Endgame D3.b 2b-iii-d inc 1 (2026-08-23) - validator arm 2: label
## in-edge agreement as a machine check

WHAT: at every fragment-local branch patch, the source-side register
state must equal the landing position's - the guarantee the
linearization-point discipline makes BY CONSTRUCTION, now asserted so
a future scan, snap or seam change that breaks the discipline aborts
by name at compile time instead of running a stale register. The
signature folds the GP cache's (reg, slot) pairs (the per-pc-varying
state; fcache/scache/tflush are run-constant in both modes); a branch
patched to seam_pre runs the pc's transitions and compares against
the PRE-transition signature, one patched to the label against the
post-transition one. Per-fixup signatures are recorded in the main
loop after each op (no Fixup struct change - fixups only append, so a
resize-to-current covers exactly the op's own branches); label and
pre-transition signatures at their recording sites. The check runs
under BOTH modes - it validates the ShareSeams as much as the lsra
transitions - and passed over the whole suite and corpus in all four
lever x arena configs, which is the discipline's first machine-veri-
fied sweep.

WATCHED FAILING: jit_lin_point sabotaged to admit one crossing edge -
the very next lever -rt run aborts with the arm's named message.

⛔ THE cc1f50f SHAPE, TWICE IN ONE CHANGE: the check block is
validation only, so it is gated #ifndef NDEBUG - and first `want`,
then `to_pre` went set-but-unused in the ASSERTS=0 clang lane
(-Werror). The pre-push battery's clang OPT=1 ASSERTS=0 LTO=0 step
caught both before CI could.

Verified: vdjcmp 116/116 (bookkeeping adds no bytes), the full
lever x arena -rt + corpus matrix, TESTS=1 OPT=1 both lever states,
clang OPT=1 ASSERTS=0 LTO=0 zero warnings, census gate.

## Endgame D3.b 2b-iii-d inc 2 (2026-08-23) - the whole-run fallback
## restated on interval FACTS; 43_sieve's +37% was an idle prefix

WHAT the broad ledger found (13 benches; the six-bench set had a
corpus hole - the standing lesson, again): 43_sieve +37.27%,
14_array_subscript +2.48%. The first theory (a reg neighbour blocking
the trans-mode extension) was WRONG - trans mode never engages on 43
at all (hoist regions decline it); the cost lived in the WHOLE-RUN
BRIDGE FALLBACK, and LSRADBG on that branch showed it: the sieve's
inner counters enter their loop-body run LIVE-IN and untouched for
the first pcs, that idle prefix is not a candidate piece (no use
inside), and the fallback's all-pieces-resident rule therefore
excluded the run's HOTTEST slots - a rule stricter than the pick,
which never cared about idle stretches.

THE FIX: the whole-run fallback is now the pick's contract restated
directly on the interval facts - int evidence (wint > 0, the m3
rule), NO mem event on any interval (the d1 rule, the pick's bad()),
no float facts, the >= 3 floor - with no detour through the plan's
pieces at all (the fallback no longer calls jit_lsra_assign; the
plan is a TRANS-mode input, and reading whole-run answers off its
pieces was the category error). MEASURED: 43 +37.19% -> +0.45%,
14 +2.48% -> +0.31%; the 10-bench ledger reads +0.12%..+1.24%
(81_regs_int_14's +1.24% is the residual class).

Rider: the trans-mode start-extension gained REASSIGNMENT (a piece
whose extension the reg neighbour blocks moves to a register free
over the extended span, lowest-first) - correct by the snap model's
machine checks and green everywhere, but its REACH is unverified (43
was not its shape after all); noted, not claimed.

Verified: full lever x arena -rt + corpus, vdjcmp 116/116 default,
TESTS=1 OPT=1 both lever states, clang OPT=1 ASSERTS=0 LTO=0 zero
warnings, census gate.

## #99: the arity-1 already-boxed VmInvoker::call overload (2026-08-23)

The 2026-08-16 regression check's 67_make_dict +5.91% was bisected to
202f816 (the ONE-entry callback unification) and diagnosed to the
line: make_dict passes an already-boxed `const EvalValue k`, and the
template's single-boxing `EvalValue argv[] = { EvalValue(args)... }`
COPY-CONSTRUCTS it - a full EvalValue copy + retain/release per
callback that the old `inv.invoke(&k, 1)` never paid. "Boxed exactly
once" is true for RAW C++ types (the 34_sort -16.2% win); an
already-boxed lvalue was boxed twice in effect.

THE FIX: a non-template `call(const EvalValue &)` overload forwarding
the POINTER (no copy; same tier selection; same argument contract).
Overload resolution does the routing: a const lvalue prefers the
non-template; an RVALUE EvalValue prefers the template's && binding
(move into argv); raw types only match the template - so the
single-boxing contract is unchanged everywhere else. Arity >= 2
cannot use the trick (invoke's argv is contiguous; two separate
lvalues are not).

MEASURED (isolated HEAD-with vs HEAD-without, callgrind, OPT=1
ASSERTS=0): 67_make_dict -3.04%; 39_find_builtin, 35_map_filter,
34_sort_custom_cmp all +0.00% exactly. The isolation step earned its
keep: a first comparison against 202f816 itself showed 34 "-1.1%"
and 39 "+0.57%" that were unrelated drift across the intervening
weeks. RESIDUAL, attributed and open: 67 remains +2.4% vs
pre-202f816 - +12 Ir/call inside the unified invoke's bind path
itself, not a copy (task #99 carries it).

Nets: -rt 1690 x 5, corpus_diff. No emission change.

### #99 closes: 09_fib's +4.68% fully attributed (2026-08-23)

The #93-window half (+2.52M, 202f816 -> aa3220f) is COMPILE TIME, not
codegen: the bytecode is byte-identical across the window (-nj -vd
diff: zero lines), the -vdj diff is only baked addresses (those shas
predate the reproducible dump), and a fib(1) variant - same compile,
negligible run - carries +2.59M of the delta. It is the escape
analysis itself (esc_collect's body walks + the call-graph fixpoint
over fib's clones): +3.8% of fib's ~69M-Ir compile, the analysis
price of #94's measured runtime win, and largely made of the same
for_each_child dynamic_cast chains task #102 targets - #102 will
shrink it. The devirtualization suspicion (written_slots -> slot2fn)
is DEAD: the bytecode did not move.

The #94 half (+1.96M to the then-HEAD) stands as diagnosed in
2026-08-16: vm_bind_arg's per-bind borrow test, ~0.28 instr/call over
~7M calls - the flip side of 76_funcval_dispatch's -13.5%. The
growth SINCE then (~+2.9M) is the D-arc's RTTI-strcmp compile drift,
attributed separately this session.

09's full decomposition: 88.64M + 2.52M (#93 compile, -> #102)
+ 1.96M (#94 bind test, accepted) + ~2.9M (strcmp drift, -> #102)
= 96.03M at HEAD. Open decisions for the maintainer: the 67
invoke-internal +12 Ir/call residual (fix vs accepted trade), and
whether #93's compile price stands as-is until #102.

## Endgame 2b-iii-d inc 3 (2026-08-23) - 81 diagnosed to ONE
## instruction; the split-worthiness floor VINDICATED and landed

THE DIAGNOSIS (the 43 toolkit, evidence before theory): 81's +1.24%
is per-iteration (+2M per scale unit), and the loop bodies differ by
EXACTLY ONE instruction - 74 vs 75 (1/74 = +1.35% = the measured
+1.36%). The instruction: a10 served from a spill home ([rbp-0x38],
one tagless access) under the pick vs from the FRAME with a type
stamp under the lever. The chain: a conflict RETRY denies one pool
register; at K-1 the scan's i evicts a10 at its own install (pc 14),
keeping a 1-use prefix [10,14); the remainder covers the loop; the
home filter excludes a10 ("has a resident piece") -> frame + stamp.

THE VINDICATION, owned in full: this is EXACTLY the shape the
split-worthiness floor addressed - and the floor was REVERTED
earlier today on a "measured flat" reading that re-measurement now
proves FALSE (with the floor, 81's per-unit Ir is 147,000,011 -
IDENTICAL to lever-off; the earlier flat number was a measurement
error, most likely a stale binary). The floor is re-landed: an
eviction keeps its prefix only when it carries >= 2 int touches,
else the loser demotes WHOLE (no install, no flush, home-eligible).

MEASURED (floor in, lever-on vs off): 81 +1.24% -> +0.27%,
83 +0.72% -> +0.37%; the six-bench spot set reads +0.20%..+0.45% -
the residual band is the lever's COMPILE-side analysis cost, no
longer loop-side codegen.

NETS: property H pins the floor (an eviction's kept prefix carries
>= 2 int touches; a mem-CUT successor is exempt - its flush is
data-carrying) - WATCHED: removing the floor fails H by name on the
K=1 case ("eviction kept a 1-use prefix of slot 1 [5,7)"). The K=1
z-property is restated a THIRD time (max residency over all slots
must exceed idle-z's) - the picked-based and weight-based forms both
conflated something. ⛔ HONESTY NOTE: the evict-FURTHEST policy
itself currently has NO working watched-failing case - all three
formulations failed to pin it (temps dominate the K=1 dynamics);
a dedicated temp-aware shape is a recorded follow-up, and the policy
is otherwise exercised (not verified) by every lever run.

Verified: -rt + corpus, both configs, both arenas; vdjcmp 116/116
default; TESTS=1 OPT=1 both configs; clang OPT=1 ASSERTS=0 LTO=0
zero warnings; census gate.

## Endgame 2b-iii-d inc 4 (2026-08-23) - THE PICK-PARITY PAIR: the
## lever's per-iteration ledger goes to <= 0.00% everywhere measured

The maintainer's flatness mandate ("no theoretical reason to be worse
than flat") implemented as two pick-contract inheritances, one
increment:

1. THE ADMISSION FLOOR: any residency requires the slot's RUN-WIDE
   int weight >= 3 - the pick's threshold, now understood to carry
   TWO loads: soundness once (the m3 ReturnV lesson) and the
   per-ENTRY cost model (push + entry load + exit flush + pop, paid
   per fragment entry - 34_sort/73's chunks are entered per callback
   call, and the scan admitting 1-2-use slots there cost
   +3.88%/+1.81% per iteration; callee-saved pops went 0->12 and
   9->16). Property F2 pins it (no resident piece below the floor),
   WATCHED: removing the floor names slots with weight 1 and 2.

2. THE WHOLE-RUN RESCUE: a lin-demotion striking a slot the pick
   would pin whole-run (no mem_int on any interval, no float facts,
   at the floor) now merges ALL the slot's pieces into ONE resident
   [begin, end) piece on a run-free register - the pick's pin as the
   degenerate snap output, needing no transitions and therefore
   immune to linearization points (44_primes_sqrt's +3.21%/iter: the
   pick pinned r12, the snap had shredded the same slot). S3 is
   restated in COVERAGE form (the rescue erases pieces, so per-index
   comparison is structurally dead): new residency is legal only
   where the slot is dead or pick-qualifies. The rescue is vacuity-
   guarded (saw_rescue - a [0, size) resident piece over live,
   pick-qualified pcs), WATCHED: forcing eligibility false trips the
   guard. The crafted case took three sources: `sc = runtime(7)` in
   a branch lowers via a temp + MoveV (a mem event - correctly
   ineligible), argv is unregistered in the -rt harness (FIX-1
   refuses it), `sc = int(runtime(7))` lowers with the call target
   direct and rescues.

MEASURED (same-binary env A/B, callgrind, per-iteration =
(s3-s1)/2): 34 +3.88% -> +0.00%, 44 +3.21% -> +0.00%, 73 +1.81% ->
+0.00%, 86/87 -> +0.00%, 07/43 hold at +0.00% - and 81 -> -2.72%,
83 -> -0.98%: the lever now BEATS the pick on the register-pressure
family, the arc's first per-iteration wins. Residual s1 deltas
(+0.02..1.17%) are the compile-side band plus 69's heap-priming
class (attributed separately: byte-identical emission, malloc bin
walks from the analysis' transient allocations).

Verified: -rt + corpus, both configs, both arenas; vdjcmp 116/116
default; TESTS=1 OPT=1 both configs; clang OPT=1 ASSERTS=0 LTO=0
zero warnings; census gate.

THE FULL 88-BENCH SWEEP (2026-08-24, callgrind s1+s3, lever
off/on, -npc both sides): ZERO per-iteration regressions. Wins
over the 0.30% band: 06_if_branch -16.80% (the scan pins r14/r15
across the branchy body the pick declines - 3 fewer helper calls,
9 fewer tag stores per loop), 81 -2.72%, 82 -1.56%, 83 -0.98%,
68_nested -0.46%. The one positive entry, 69_exc_crossframe
+4.13%, has BYTE-IDENTICAL emission both ways - the heap-priming
artifact class (69 allocates an exception every iteration, so a
differently-primed malloc free list scales with iterations).
Everything else +-0.00%/iter; the +0.0..1.2% s1 residue is the
compile-side band. Sweep hygiene rule re-learned: an ad-hoc
callgrind sweep BYPASSES bench/run.py and so must pass -npc
itself - the first sweep did not and was discarded (09_fib's
4,820 Ir/2sc per-iteration denominator was the tell).

## Endgame F1+F2 (2026-08-24) - THE FLOAT/XMM TWIN's analysis half:
## the FltEvent stream and the scan's FLOAT mode

Pure analysis, no emission change (the bridge still leaves fhot to
the pick in both modes; F4 is the consumer). F1: jit_qualify_
intervals exports a FltEvent {pc, slot, read|write|mem} stream,
self-contained for cutting (mem entries from bad() AND badf(); a
badi() must never cut the float side). F2: jit_lsra_assign's
trailing `fev` param flips the pool - cuts, evidence, the >= 3
admission floor and the split-worthiness count all derive from the
stream; an interval with any countable int use is disqualified
(uses_ret exempt).

⛔ THE EVIDENCE ASYMMETRY, the one real rule difference vs the int
side: a usef READ is not float type evidence - a float op
legitimately reads a definitely-int slot through the promote arm,
and a float entry-load movsd would reinterpret its int payload as
a double. Per-piece admission is therefore WRITE-FIRST: a float
write must exist with no read at a strictly earlier pc (a same-pc
read+write pair is one dst touch, admissible).

Findings: LoadImmFloat is a float WRITE (usef+fdst, badi only), so
an uncut float accumulator is write-first FROM ITS DEF and stays
resident - the conservative refusal only bites a slot whose
interval a float-side cut (a DictStore key, a MoveV dest) splits
between its def and a read-before-write region. A badf-ONLY site
does not exist today (every visitor badf() pairs with a bad()), so
badf's own event push is documented DEFENSIVE. And a read-first
VACUITY detector must run per PIECE - an interval's early def
write hides the shape at interval granularity.

Nets: property E-float (stream reconciles with the per-interval
facts; WATCHED - mislabeling a read as a write fails), and
jit_lsra_float_check (FI1 tiling, FG write-first, FD disjoint
pools, FF2 floor; WATCHED both ways - accepting read-first names
the piece, inflating the floor weights names the slots; four
vacuity-guarded refusal shapes).

## Endgame F3 (2026-08-24) - the snap's FLOAT mode

jit_lsra_snap gains the same trailing-fev convention as the scan:
non-null flips the WHOLE-RUN RESCUE's eligibility to the pick's
fhot rule (sum uses_float >= 3, wrote_float somewhere - the pick's
fdst requirement, whose whole-run soundness note is the argument -
no mem_float anywhere, no countable int use on any interval). The
lin-point machinery, demotion, translation and the occupancy model
are register-file-agnostic and shared verbatim.

The rescue's float test shape is the int one's twin with one
lowering fact doing the work: `sc = float(runtime(7))` in a branch
lowers to a CallBuiltinV whose DST is sc - a BARRIER (bracketed
flush/reload), not a bad() - so sc keeps a mem-free float ledger
and stays pick-eligible while the branch edge makes its piece
boundary a non-lin point, demoting it; the rescue must then merge
sc whole-run. Net: jit_lsra_snap_float_check (FS2 replay, FS5
single-residency, the rescue + vacuity). WATCHED: forcing
float-mode ineligibility fails the vacuity guard by name.

## Endgame F4a (2026-08-24) - the bridge's FLOAT fallback: lever-on
## fhot from the interval facts, exact pick parity

With the lever ON, the float pin set (fhot) now comes from the lsra
analysis instead of the pick: the pick's fhot rule restated on the
interval FACTS - sum uses_float >= 3, wrote_float somewhere, no
mem_float anywhere, no countable int use; temps excluded;
weight-ranked, capped MAX_FCACHED. Deliberately NEVER a detour
through the plan's pieces: a live-in float slot's idle prefix is
not a candidate piece, and an all-pieces-resident rule would un-pin
it (the 43_sieve +37% lesson, on the GP side). The emission
downstream (ftake_reg + fload, barrier brackets, exit flushes)
consumes fhot unchanged; per-pc xmm pieces are F4b's trans-mode
work.

MEASURED PARITY, the increment's whole claim: lever-on emission is
BYTE-IDENTICAL to the pick's over all 116 corpus programs (vdjcmp,
OPT=1 ASSERTS=0 so the new counter's TESTS-only bump is compiled
out - on a TESTS build the bump itself perturbs the dump, which is
worth remembering when using vdjcmp as a parity oracle). Default
config also 116/116.

Execution proof: g_jit_lsra_fpins (a JITSTATS row), bumped per
entry of a float-pinned fragment whose fhot the lever chose;
WATCHED - skipping the fhot replacement leaves every value right
and fails only the counter assertion in jit_lsra_bridge_check.

## Endgame F4b core (2026-08-24) - FLOAT trans mode: per-pc xmm
## pieces execute through e.fcache

The F2/F3 float scan+snap become emission consumers behind the
lever: the bridge attempts float trans mode (same v1 bounds as GP -
no temps, hregs empty), fhot becomes the entry-occupant list,
afphys binds abstract regs to FCACHE_REGS, the seam loop executes
float transitions (interior-end flush = fstore + t_float tag store,
install = fload; fgive/ftake_fixed keep busy <=> entry per pc), and
entry stubs replay base_fcache + the transitions at or before their
pc - e.fcache at stub-emission time is the post-all-transitions
FINAL state, the inc-2 lesson on the float file. A transitioned
slot leaves textra_f and fread_raw (one machinery per slot). The
barrier brackets and exit flushes read the evolving e.fcache and
needed no change - register-state-driven, as designed.

⛔ THE BINDING IS TWO LOOPS - take every occupant-less areg's
register FIRST, then give back: an fgive inside the take loop hands
the SAME physical xmm to the next areg (both bound to xmm4), and
the seam's evict then finds the other slot on the register -
watched aborting `c.slot == tr.evict_slot` on 01_float_chain_ref_
temp before the fix. The GP aphys block had this structure all
along; the float copy initially did not.

Verified: -rt + corpus both configs, the off-arena lever config,
default vdjcmp 116/116 byte-identical. Real float transitions
execute on the corpus (01_float_chain: installs at pc 9/11, evicts
at 22/28, across two runs). Remaining in F4b (plan marker): a
float-specific transition counter + watched sabotage, a bridge
float-handoff case, and the F5 measurement.

F4b CLOSING (same day): g_jit_lsra_ftrans - the float-specific
execution proof (the GP g_jit_lsra_trans conflates the pools, and a
float-seam test satisfied by a GP transition proves nothing) - is a
JITSTATS row bumped only from the emitted float seam. The bridge
check runs the 01_float_chain shape and requires it to move;
WATCHED: declining float-tmode adoption leaves 1953/1954 green
(every value right - the F4a fallback serves) and fails only the
counter assertion. Reach on the float benches (lever on, -npc):
04_float_arith executes 2 float transitions per run and
55_float_sum 4; 54_mandelbrot/88 enter with lsra float pins and no
transitions; 46/79 decline. From here lever-on float emission
legitimately differs from the pick's, so the oracle is corpus_diff
plus F5's per-iteration Ir - byte identity retired its role at
F4a.

F5 CLOSING (same day): the float spot set (04/46/54/55/79/88 +
05/40) is +0.00%/iter everywhere with the twin fully active, and
the FULL 88-bench -npc sweep at HEAD reproduces the pre-twin
ledger exactly - the same five wins (06 -16.80%, 81 -2.72%, 82
-1.56%, 83 -0.98%, 68 -0.46%), 69's byte-identical heap-priming
artifact, everything else +-0.00% per iteration. The float twin
(F1..F4b) is COMPLETE and corpus-flat. The honest gap: no wins
from float transitions yet, because the corpus has no float
register-pressure shape (the regs family is int-only) - today's
float transitions run entry-adjacent, invisible per iteration; a
phased 5+-hot-float-local bench is the shape that would show the
payoff.

## C2b-regions arc step 1 (2026-08-24) - the ShareSeam cold-path
## bypass closed, and the hazard mapped smaller than feared

The C2b-into-tmode plan marker asked whether today's ShareSeams
already carry the fresh-window hazard. Answer, from probes now kept
as MYLANG_SHAREDBG: the corpus has ZERO seam/region coexistence
(latent, not live); IN-region seams were already impossible (a
region lives inside a loop whose back edge makes every interior pc
a non-lin-point); and the ONE reachable hazard pc is exactly L+1 -
outside the loop and lin-LEGAL, yet the cold copy's rejoin jumps to
label[L+1], PAST a seam there, because the failed-C1-guard edge is
an emission construct jit_run_edges cannot see. A seam skipped on
the cold path leaves pre-seam registers where the main stream is
post-seam.

Fix: jit_share_plan takes `noseam` ranges ([T, L+1] per hoist
region) and seam_ok refuses them, counted by g_jit_share_clamped.
The -rt net (jit_share_region_clamp) constructs the L+1 shape - an
early-phase pin whose span frees before the loop, and a post-loop
slot whose lo lands exactly at L+1, rank-first among the overflow,
under MYLANG_JIT_MAXPINS=1 + the in-process rshare force - and
requires the clamp to FIRE. WATCHED: removing the clamp fails only
the counter assertion; the value-level wrong answer needs the C1
guard to FAIL at runtime, which no lever can force today (the
cold-arm forcing lane, designed-not-built). Default emission
116/116 byte-identical (no corpus program has the coexistence).

The finding also sizes the remaining tmode lift: transitions inside
a region are impossible by the snap's own lin rule, so lifting
hregs.empty() needs only the snap's [T, L+1] refusal (the noseam
mirror) plus the cold copy emitted against the replayed state at T.

## C2b-regions arc step 2 (2026-08-25) - the tmode-lift machinery
## lands (gates closed); the cold-copy fresh-window fixed for real

jit_lsra_snap gains `noreach` ((T, L+1] per region; a transition AT
T is legal - emitted before the guard, both paths see it), and each
cold copy is now emitted against the REPLAYED guard-point cache
state - base + ShareSeams <= T + transitions <= T, ra rebuilt to
busy <=> entry, snapshot/restore bracketing, no code emitted. The
first replay version applied only transitions; the stub replay's
rule (seams too) is the correct one, and lever-on 68_nested proves
it matters: its cold exits re-bound to the guard-point epilogue -
lever-on 68 HAS seams + regions (the coexistence scan ran default
config only; lever-on's pin/home economy differs), so the
fresh-window defect was live in its emission, dormant only because
a C1 guard never fails at runtime.

The hregs.empty() tmode gates stay CLOSED: lifting them exposed two
further interactions - the C2b pair's DISPLACE branch pops pins
from `hot` after the plan spent them (under tmode `hot` IS the
abstract-reg zip; displaced pins stay "resident" but unloaded), and
an open wrong-value on 17_elem2_divmod_roles (mods 1531 vs 513, no
transitions involved - the tmode pin set meets the ElemScratch
divisor-gate roles; the cold replay is bisected OUT). Both recorded
in the plan with the repro; shipping and lever-on corpus behavior
are unchanged except the 68 epilogue re-bind.

## C2b-regions arc step 3 (2026-08-25) - the elem2-divmod wrong
## value fixed (RefScratch's excl); the xrot matrix finds a
## pre-existing lever defect

The open wrong value is diagnosed and closed: RefScratch granted
the ref-check scratch a register the allocator sees as FREE but
which holds the op's ISA RESULT - idiv leaves the mod remainder in
rdx, the encoder's claim is transient, and with rcx unavailable
the grant handed rdx to the type-tag load, so jit_put_int received
the type pointer's dword as the value (mods 1531 vs 513 on
17_elem2_divmod_roles). Only the CALLER knows where the value
lives: emit_ref_check gains `excl`, passed by store_dst and both
of store_dst_bool's scratches; when even the preferred register is
excluded, the push-borrow arm serves - push/pop restores the value
before use, so borrowing an excluded register is safe where
granting it is not. Verified 513 under the lifted gate with the
whole lever-on corpus green; default emission 116/116 unchanged.
A latent hole any dense-enough pin set could reach.

The pair's DISPLACE branch is now forbidden under tmode (leftover-
only) - the zip-corruption diagnosis stands at the site.

The gates STAY closed on one remaining, PRE-EXISTING defect the
lever-on xrot matrix found (it had never been run): rotation 4
puts rax first, a lever pin spans the elem2 fused read, and its
raw `mov rax, [rcx+r9*8]` never asks the Phase-A conflict
machinery - JIT-REGTRACK aborts at compile, the tripwire doing
its job. Repro: MYLANG_JIT_LSRA=1 MYLANG_JIT_XROT=4 on
tests/functional/16_elem2_fused.my, at committed HEAD. The fix
arc is the elem2 raw-rax sites joining ask-and-evict, then the
re-lift.

## C2b-regions arc SOLVED (2026-08-26) - a failed tmode install now
## declares its conflict; the gates lift with 43 at exact parity

The 43_sieve chase's end: of five two_addr candidate sites, one
missed - j's op in the MAIN stream (dreg=-1) while its cold copy
printed dreg=10. The aphys binding gave j's abstract register r10
at BIND time, before the C1 region's B3 claim took r10 at its
entry; the install's take_fixed then failed, and the seam arm's
documented "skipping is safe" silently abandoned the piece the
plan promised - the hot op ran STAGED 2.2M times, making tmode's
pins worse than no pins at all (the MAXPINS=0 triangle). The fix:
a failed install DECLARES the conflict (a pin_conflicts bit), the
pass re-emits with the register denied, and the binding picks
another. 43 lever-on: 42.5M Ir/iter, exact fallback parity;
two_addr_reg restored to 2,201,542; 46's -5.5%/iter win retained.
The refuted served-weight cost gate is deleted.

Both hregs gates are lifted for real: -rt + corpus in both
configs, the lever-on xrot matrix on every rotation, the off-arena
lever config, the census gate, and default vdjcmp 116/116 - all
green. What remains for #96's endgame: the clean lifted-config
wall A/B, then the flip gate.

## #96 endgame - the clean wall A/B (2026-08-26): geomean 0.997x,
## the lever's first net full-suite wall win

RULE B1 observed end to end. Interleaved lever-on vs lever-off at
the lifted HEAD over 87 benchmarks: geomean cur/base 0.997x - the
allocator now WINS the wall suite-wide. Wins: 85_regs_ref 0.73x,
46_matrix_mult 0.82x, 82_regs 0.83x, 18_foreach 0.85x, 83 0.91x,
84 0.91x, 06_if_branch 0.93x. 43_sieve is gone from the tail; the
worst entries (59 1.08x, 52 1.07x) were Ir-spot-checked and are
EXACTLY identical per iteration in both configs - single-run wall
noise on sub-100ms benches. The ledger for the maintainer's flip
decision is complete on both axes.

## ⛔ D4 - THE DEFAULT FLIP (2026-08-26, maintainer's decision)

MYLANG_JIT_LSRA defaults ON: the linear-scan allocator (whole-run
pins, per-pc pieces with transitions, spill homes, the float twin)
chooses the register plan for every fragment. MYLANG_JIT_LSRA=0 is
the debugging OPT-OUT and restores the legacy pick end to end - a
same-binary A/B for any suspected allocator bug, the same role -nj
and --no-opt play one layer down and up.

The decision ledger, complete on both axes at the flip: Ir - zero
per-iteration regressions across all 88 corpus benches, wins
06_if_branch -16.8%, 46_matrix -5.5% (post gate-lift), 81 -2.7%,
82 -1.6%, 83 -1.0%, 68 -0.5%, 43_sieve at exact parity after the
install-conflict fix; wall - geomean cur/base 0.997x over 87
(RULE B1, interleaved), wins to 0.73x on the pressure/region
family, the residual tail Ir-verified as single-run noise.

Coverage of the OPT-OUT config: the flip battery runs -rt +
corpus with MYLANG_JIT_LSRA=0, and the coverage tests pinning the
pick's own mechanisms (jit_xcache_pins, jit_hoist_pair_conflict,
jit_range_share_test, jit_spill_homes, jit_rax_pin_test) force
g_jit_lsra = false for their duration, so the pick cannot rot
silently while it remains the opt-out. Default-config emission
CHANGES with this flip, by design - vdjcmp against a pre-flip
binary reports the allocator's differences, and byte-identity
retires as the default-config oracle in favor of corpus_diff +
the per-iteration Ir ledger.

## Density-aware eviction (#103a, 2026-08-24)

The scan's pressure contest chooses its victim by USE DENSITY over
the remaining interval (uses-remaining / span-remaining,
cross-multiplied, newcomer competing; ties evict the smaller slot -
the pre-density tie direction), replacing furthest-next-use.

WHY: 89_regs_float_08 - the phased float-pressure oracle - measured
the lever +6.5% Ir/iter WORSE than the pick on the very shape the
float machinery was built for. The diagnosis (one slot): every
float's interval spans prologue-def -> sum-read, so at fj's walk
arrival the pool was full and fj had the FURTHEST next use (its
loop was ~18 pcs away) - the newcomer stayed memory for its whole
interval, paying a tagged store + four reloads per phase-2
iteration, while the pick's static COUNT ranking (which IS a
density ranking) pinned fj in xmm6 and won. f3's whole-run
demotion was the same miss: distance evicted the 2-op/iter
recurrence to keep the 1-op index.

MEASURED (Ir/iter, OPT=1 ASSERTS=0, -npc, old-on vs new-on over
the 88-bench corpus): 86 byte-identical, 89_regs_float_08 -6.10%
(exact pick parity - the whole regression erased), 68_nested
-0.12%. Zero regressions. Wall on 89: 176ms vs 196ms best-of-5,
interleaved twice (0.90x). Off-config (the pick) untouched by
construction.

PINNED by the `density beats distance` jit_lsra_check case - the
89 microshape at K=1 (a near-next-use sparse holder vs a far dense
newcomer); watched failing under a next-use sabotage ("dense z
holds 0 pcs, max 8").

WHAT THIS IS NOT: parity, not the win. Interval SPLITTING WITH
LIFETIME HOLES (split fj to its hot phase piece AND split the
phase-1 pins' idle gaps so a register is actually free to receive
it) beats both allocators on this shape - each phase would pay ONE
memory slot where the pick pays two + four. That is the
second-chance revival, recorded in the plan as the (b) follow-up.

## Lifetime holes (#103b, 2026-08-25)

The scan splits a candidate piece at a use GAP that contains a
FULL loop (a back-edge with target and source both inside the
gap): the register is released across the hole and someone else's
hot region can live there. Gaps are found at the piece HEAD
(post-cut pieces often start at a call boundary with the first
use a loop away), between uses, and at the TAIL (a pure trim).
Hole boundaries are placed on LIN POINTS by the assign itself
(jit_run_edges + jit_lin_point) - the snap's extend-or-demote
slide is bounded by the same slot's neighbouring piece, i.e. the
hole, so a boundary the snap cannot accept in place would DEMOTE,
not slide. The full-loop rule doubles as the churn guard: a gap
inside one iteration contains no complete loop, so a hot body
never gains a per-iteration seam. A split side keeps candidacy
only with >= 2 events inside.

ON 89 IT DELIVERS THE PHASED IDEAL - each phase serves four of
its five floats (f1/f7 the single losers), fj holds a register in
its loop - and the honest ledger is: Ir FLAT (every memory ref
became a one-instruction reg move), DATA READS -40% (60.4M ->
36.4M at scale 3), wall NEUTRAL vs density-only once fmov_rr's
false dependency was fixed (the movaps commit - found BECAUSE the
hole plans turned loads into reg-reg moves and the merge
serialized the body: drefs -40% yet wall 1.43x WORSE was the
smoking pair). Corpus Ir: 86 of 88 byte-identical,
17_array_concat -0.42%, 75_indexed_unpack -0.90%, zero
regressions.

PINNED by the `lifetime hole frees the loop` jit_lsra_check case
(K=1, two sequential loops, b's piece starting at a call boundary
with its first use a full loop away - only a HEAD hole can serve
loop 2); watched failing with the pass disabled ("b resident 0
pcs"). Building the case found a real oracle subtlety: the first
shape produced an exact density TIE (cu/cs 3/3 vs 4/4) and the
tie rule kept the active - the MYLANG_LSRADBG2 contest probe in
jit_lsra_assign is what surfaced it, and it stays.

## The second-chance re-queue (#103b-2, 2026-08-25)

The walk is event-driven now: piece STARTS and RE-BIDS process in
(pc, kind, slot) order. A contest loser - a denied newcomer, a
whole-demoted active, an eviction remainder - PARKS and re-bids at
the earliest lin point after the loss (one pending re-bid per
piece; candidacy re-checked with the >= 2-events floor, and events
only shrink with pc, so a first failure is final). A won re-bid
splits its piece: [s, at) stays memory, [at, e) takes the
register. Lin-ness bounds everything: a loop body has no lin
points, so a loser cannot ping-pong inside one, and a re-bid start
is a legal install pc by construction (the snap's own rule).

WHY IT EXISTS: the walk visits each piece once, at its start, so a
loser's verdict was sealed the moment it lost - when the winner
DIED mid-run the freed register went to whoever arrived next,
never back to the still-live loser. The residual shape (no full
loop in the loser's use gap, so the hole pass cannot pre-split):
b touched sparsely in loop 1, hot in loop 2, out-densitied by a
in loop 1 - b re-bids at the post-loop-1 lin point and takes the
register for loop 2, out-densitying the loop-2 counter.

THE HISTORY IS THE POINT: this was built once during #96 and
REJECTED at +7.9%/iter on 83_regs_int_40 - under furthest-next-use
eviction, re-queued pieces churned (install, re-evict, seam cost
each round). The density contest closes that direction by
construction: a sparse re-entrant cannot displace a dense holder.
MEASURED on the revival: 87 of 88 corpus benches byte-identical
per-iteration, 68_nested +0.10% (8 re-bids in its plan - named,
accepted), 83's plan takes 2 re-bids at ZERO per-iteration cost -
the exact bench that killed v1. Wall flat on 83/89/68.

PINNED by `second chance serves the survivor` (K=1, the residual
shape; asserts b's residency AND plan.rebids >= 1 - the vacuity
guard); watched failing with park() neutered ("b resident 0 pcs" +
"rebids == 0"). Reach: `lsra_rebids` (JITSTATS) - 68_nested and
83_regs_int_40 on the corpus.

## #98 - the opcode-table census, and the three holes it closed
## (2026-08-25)

THE AUDIT the lever-A whitelist episode demanded: walk EVERY table
that is keyed by opcode and gates an OPTIMIZATION - the class that
fails silently when an op is added (unlike verify_chunk, whose
no-default switch fails the build). Six tables audited, each first
DIFFED against the full opcode enum, then reach-measured over the
corpus before deciding anything:

 - `pick_visit_op` (register caching / LSRA qualification): the
   CallBuiltinLV/LVElem/LVMember family was UNLISTED, so one
   sort/pop/insert/erase turned caching off for its WHOLE run - the
   exact silent shape MathFnV and CmpIntV were once found in. FIXED:
   the CallBuiltinV rule applies verbatim (a callback can mutate
   slots the emitter cannot enumerate; none of the three is a
   branch), so they BRACKET (flush/reload) now. Reach: 7 corpus
   programs. The CALL family (CallV/CachedCallV/CallValueV/
   CallValueGenericV) stays unclassified DELIBERATELY - recorded in
   the census rows: a run with a MyLang call caches nothing, and
   whether a call should be a barrier instead is #97's call-protocol
   decision, not a drive-by.
 - `op_fully_native` (delete-originals): StructCtorV (unplanned) and
   StructCtorBoxedV were left undecided by the #56 batch - both are
   convey-only, so both are DELETABLE now. The unplanned POD ctor
   shares MakeStructArrayV's emit (cold-side exc-stamp included) and
   met the bar all along; the boxed ctor needed its emit's exc-stamp
   and the `catch (...)` eptr net ADDED (a #142-class hazard on its
   own: its twin had the net, it did not - a plain Exception through
   the noexcept would have been std::terminate). CallValueGenericV's
   absence is CORRECT (it bails: depth cap / chunkless callee) and
   now recorded.
 - `op_writes_pure_target` (arg-staging retarget): candidates exist
   (LoadMemberInt, the DictLoads, MathFnV...) but a corpus scan of
   leftover `<produce t>; move X = t` pairs shows the unlisted
   producers at <= 7 sites, none in the fusable arg context - no
   admission owed. The specialized-arith family's absence is the
   STAGE trap (the ops do not exist at emit_args_range's stage),
   recorded in the census header instead of repeated.
 - `bc_inline_op_ok` (the splice whitelist): MYLANG_INLAUDIT=1 over
   the corpus shows ZERO call sites blocked by an op - the blockers
   are runtime callees, tail shape and size. No admission owed.
 - `jit_op_eligible` / `op_never_exits`: clean - the only enum
   members without cases are the calls (op_run_eligible's layer, by
   design) and the EnterNative/ExitBlock meta-ops.
 - `op_is_simple_island`: the documented-dead M3 list (the gate asks
   op_run_eligible first and the nativize-ops arc exhausted its
   sources); the census pins the historical list as-is.

THE NET: `opcode_table_census` (tests.cpp) - jit_fwd_family_coverage's
enum-derived ratchet, widened to the WHOLE opcode enum x six live
predicates (exported as jit_test_* / bc_test_* / bc_inline_op_ok
shims - forwarders, no second copy to drift). One row per opcode, six
claims per row, checked against the live predicate in BOTH directions;
adding an opcode without deciding fails with the opcode NAMED. Watched
failing four ways in one sabotage build: a deleted row (the ratchet),
the MathFnV pick case removed (the historical C2a gap), the LV barrier
removed, and the struct-ctor deletability reverted - each named its
op, and the boxed-ctor row's note names the THREE pieces (fully_native
entry + emit exc-stamp + eptr net) that must revert together or fail.

## #100 - multiply-by-constant strength reduction (2026-08-26)

div_magic's sibling (#91), split across the two levels the task named:

- **BYTECODE**: `x * 2^k` -> `IntShlRI k` in specialize_arith_ops, so
  both engines take the shift and every downstream table already
  agrees (IntShlRI predates this - no census row moved, no format
  bump, no new opcode). EXACT for every int64 x: bit_shl with a
  count in [1,62] is the plain shift and x*2^k == x<<k mod 2^64
  under -fwrapv; neither form can throw. m == 0/1 and non-powers
  stay IntMulRI.
- **JIT** (mul_plan / emit_mul_plan, one decision + one emitter like
  div_magic's pair): lea r,[r+r*s] for |m| in {3,5,9} (+neg),
  shl+neg for -2^k, lea+shl for {3,5,9}*2^k, and mov+shl+add/sub for
  2^k+-1 - the last is THE hot family: h = h*31 + c, the string-hash
  recurrence, where imul's 3-cycle latency sits on the dependency
  chain and the planned form is 2. Wired at both literal-multiply
  sites: the PINNED two-address arm takes only the scratch-free plans
  (a scratch window there could abort a rax-pinned run - the
  one-window rule; imul is one instruction on a pin, so the 2^k+-1
  decline costs a cycle at most), the GENERIC arm serves everything
  through the case's hold()/tmp. A new general-scale
  `Emitter::lea_scaled` (serves rbp/r13 bases via mod=01+disp8(0);
  RSP-index refused) is the one new encoder; every form goes through
  tracked encoders and none reads flags.

DELIBERATELY DECLINED, each re-measurable: negate on the 3-op plans
(latency parity with imul), and the movabs+imul >imm32 path (cold
scale arithmetic in every corpus hit).

REACH (corpus census before building): *2 at 30 sites, *3 at 16
across 8 benches, *31/*17 in 68_nested and 86_elem_arith_compound;
`g_jit_mul_strength` is bumped from the EMITTED code (TESTS builds),
so reach is execution-proven per program.

NETS: tests/functional/19_mul_strength.my - the exhaustive multiplier
x value matrix (every plan shape incl. the imul fallbacks and a
>imm32 constant, both INT64 extremes, wraparound: INT64_MAX * 3 must
match the tree-walker bit-for-bit, which is what pins the mod-2^64
identity of every planned sequence); the `jit: #100 mul strength`
-rt entry (the bytecode rewrite checked STRUCTURALLY on the compiled
chunk, values tree-walker-oracled, the counter's vacuity guard - the
guard caught its own first test case dying to DynRequiredEx while
both engines "agreed" on empty output). WATCHED FAILING four ways in
one sabotage build: a wrong lea scale, shl_sub emitting add, the
pinned arm's save gate dropped (mov d,d; shl; add - garbage on a
pinned *31), and the bytecode rewrite deleted - the first three as
value divergences in BOTH the -rt cases and the corpus sweep (which
also lit 05/14/17, the older *31/*17 carriers), the fourth by the
structural check.

## #101 increment 1 - the peephole's levels 1-2, done AT THE SEAM
## (2026-08-26)

The task's four levels, smallest first; this lands 1-2. A pattern
census over the corpus's -vdj (the method, before any design): 7,337
imm32-range literals paying movabs's 10 bytes, ~1,000 staged
literal-compare pairs, ZERO adjacent store-then-reload pairs (lever A
already owns that class), 7 mov X,X. So the increment is the
immediate-staging attack, done DURING emission - where lengths may
change freely (every position is recorded as emission proceeds) - not
as a byte-moving post-pass, which would have to remap every fixup,
mark and reloc.

- `Emitter::mov_imm` - the VALUE-immediate load: `mov r32, imm32`
  (zero-extends, all v < 2^32, 5-6 bytes) / `mov r64, simm32` (C7 /0,
  the negatives, 7) / movabs (the rest). Converted: load_operand(_avoid),
  emit_branch's tmp_lit, and 91 Instr-derived direct sites (scripted).
- cmp_operand (emit_branch): a literal imm32 compare operand folds
  into the cmp - the form the PINNED arms have had since #96 inc-3,
  extended to the accumulator arms (4 sites, incl. the memory-counter
  loop bounds). Identical flags; one instruction per iteration where
  it lands.

⛔ THE RULE THIS INCREMENT EARNED, watched failing on its first run:
**movabs NEVER auto-shortens - an instruction's LENGTH must be a pure
function of the BYTECODE, never of a runtime address.** The first
version shortened inside movabs itself; most movabs immediates are
baked ADDRESSES (pool buffers, descriptors), a pool lands below 2^32
in one process and above it in another, and myv_round_trip failed
instantly: the loaded image's pools sit at different addresses than
the fresh compile's, so every following offset disagreed - the exact
-vdj reproducibility contract (vdjcmp is a plain cmp). Value/address
is a CALLER fact, hence the separate mov_imm.

FIXED IN PASSING: corpus_diff.sh's LEVERS list was itself a stale
table - argfuse/xcache/scache/rshare never joined it, so those four
levers' per-lever-off configs were tested by nothing but `all`. Synced
+ a keep-in-sync warning; `peep` added (MYLANG_JIT_OFF=peep is the
increment's A/B lever, g_jit_off_extra its in-process override).

MEASURED: emitted code -1.24MB / -3.4% corpus-wide; reach 3,103
shortened loads + 335 folded compares (g_jit_peep_short/_fold,
compile-time counts - the encoding IS the artifact, proven by
disasmcheck: 0 disagreements incl. the new short forms). vdjcmp
self-test 119/119 identical. Disasm synced in the same change: the
no-REX.W B8 arm's "resume vm pc" comment asserted an emitter fact
that stopped being true (the exit was its only producer), so it lies
no more; the arm symbolises tags like its REX.W sibling.

WATCHED FAILING three ways, individually: the sign rule sabotaged
(negatives zero-extended) crashes the suite (a truncated sentinel ->
wild read); the fold's imm32 guard dropped flips the probe's
big-bound branch (5100000000 truncates - the value case built for
exactly this); the lever made dead fails the off-config counter
check. The fold's reach needed the NO-PIN configs (g_jit_lsra=false +
the cache levers) - in the pinned config every probe compare took the
pinned arm's pre-existing imm form, and the first vacuity guard
caught exactly that.

REMAINING (the task's levels 3-4 + residue, all future increments):
reorder/rename need a decode-transform-reemit platform (the fixup/
mark/reloc remap this increment deliberately avoided); the
non-Instr-derived value movabs sites (island_pc, computed constants)
are enumerable for the same mov_imm treatment; the 7 mov X,X sites.

## #101 increment 2 - the residual sweep + THE MERGE-DEPENDENCY BREAK
## (2026-08-26): the movsd disease's bigger sibling, 2-4x on its benches

The residuals first: 106 more movabs sites converted to mov_imm after
a value-vs-address audit of every non-reinterpret_cast site (the
multi-line calls the increment-1 regex missed - 2,524 emitted sites).
The audit's judgment calls, recorded: `site` is a packed line|col
VALUE, `resume_stub` a remapped pc, `callee_arg` a slot index - all
convert; `depth_addr`/`pool` are addresses and `q[0..2]` can hold a
BUILTIN's function pointers (LoadBuiltinV's 24-byte payload copy), so
they stay movabs. mov X,X now suppressed at the mov_rr seam (8 corpus
sites, allocation coincidences). The myv_round_trip oracle guards the
whole classification: a wrongly-converted address fails it the way
increment 1's first version did.

**THE HEADLINE - the cvtsi2sd MERGE-DEPENDENCY BREAK.** The census
for levels 3-4 found 174 cvtsi2sd sites; the instruction merges into
its destination's upper 64 bits, so every convert DEPENDS ON THE
REGISTER'S LAST WRITER - the exact movsd disease the movaps fix cured
for fmov_rr, sitting on every int->float promote arm. In a float loop
whose counter promotes per iteration (`s = s + i` - 55_float_sum's
hot loop, literally `cvtsi2sd xmm0, i`), the false dep CHAINS THE
ITERATIONS. `xorps dst,dst` before the convert is recognized at
rename (no execution dependency), touches no flags, and the upper
half was dead anyway (every consumer is a scalar sd op - the movaps
argument verbatim). Emitted at both cvt seams, lever-gated.

MEASURED (OPT=1 ASSERTS=0, interleaved --baseline, full suite; the
surprising numbers re-measured per the distrust rule, and the two
apparent regressions re-measured to Ir-exact-flat + wall 1.01x -
first-run noise): **suite geomean cur/base 0.954x over 89**, from

    05_mixed_arith    0.26x     55_float_sum      0.26-0.29x
    40_math_builtins  0.54x     64_struct_create  0.43x
    88_elem_float_c.  0.72x

- 55 also confirmed by a SAME-BINARY lever A/B (peep-on ~4x faster).
The Ir ledger is FLAT to +1 instruction per convert: this win is
INVISIBLE to callgrind, like the movaps one - stalls, not counts.

THE LEVELS 3-4 DECISION, from the census (recorded so it is not
re-litigated without new data): the generic decode-transform-reemit
platform is DECLINED. Level 3's residue after lever A and the
allocator is 180 store->reload pairs at distance 2-4 corpus-wide
(~1.5/program, low heat), and latency scheduling proper is the OOO
core's job (a 500+ instruction window). Level 4's one REAL class was
the merge deps - fixed above at the seam, no platform needed. The
platform's cost (full fixup/mark/reloc remap + x86 semantic modeling,
each bug a silent wrong answer) buys nothing this data can see - the
M4b shape. Re-open with a measurement, not an idea.

WATCHED FAILING: the order-inverted sabotage (xorps AFTER the
convert) fails 17 tests - the emission order is load-bearing and the
nets see the value; a wrong-REGISTER sabotage passed everything
(structurally harmless in this allocator's layout - recorded as the
weak sabotage, the counter + disasmcheck being the real net for a
silently-missing break). peep_selfmov/peep_depbrk join the report
table; the -rt probe's float loop pins depbrk reach in both lever
states.

## #97 increment 1 - THE BOXED-ELEMENT INLINE TIER (LoadElemValue),
## 2026-08-26

**WHAT.** `fn = ops[k]` / `foreach (var e in general_arr)` - a BOXED
element read out of a GENERAL-storage array - paid a full helper round
trip (`jit_load_elem_value` -> `arr_elem_at` -> `LValue::put`, ~160 Ir):
two 32-byte boxed copies, each through a type-erased VIRTUAL, plus the
old dst value's release. This is 76_funcval_dispatch's per-iteration
feeder and the first cut at #97's disease list (func-value plumbing).
The tier emits the whole read inline: navigate (t_arr base / non-slice /
kind==general / unsigned byte-bounds), then the REFERENCE LIFECYCLE the
other inline element tiers always declined:

 - RETAIN the element first (`inc dword [pointee+0]` - see the layout
   check below), so no aliasing order can free it before the copy;
 - dec-RELEASE dst's old value, with a COLD arm for count==1 that calls
   `jit_release_slot` (the full C++ dtor semantics: slice
   unregistration, the pool free) - a plain dec on the last count is
   never taken;
 - copy 24 payload bytes + the Type* raw (3 qword loads/stores).

**THE GATES, each one a rule:**
 - `elemv_inline_ok` (JitLayout): every non-vtable pointee class must
   keep `intr_refcount` at OFFSET 0 (RefCounted first base). StrObj and
   SharedObject are PRIVATE nested types, so their offsets come from
   extended JitProbe members computed inside the class; the accessible
   three (DictObject/FuncObject/StructObject) use the fake-pointer
   idiom. A layout change flips the bit and the tier self-disables.
 - t_ex DECLINES: ExceptionObject's pointee has a vtable, so its
   refcount is NOT at +0.
 - a SLICE value declines on EITHER side: the copy must run the C++
   machinery that (un)registers it in the parent's live-slices set.
 - dst == base declines at COMPILE time; with them distinct, the base
   slot's own reference keeps the array alive through the release.
 - **THE SCALE-WRAP GUARD:** 48 is not a SIB scale, so the index is
   pre-multiplied - and `2^60 * 48 == 0 mod 2^64`, so a huge index
   would wrap back INTO bounds and silently read element 0. `imul`
   sets OF exactly when the product does not fit 64 bits: a `jo`
   declines. Watched failing: without it, `g[2^60]` returned g[0]
   instead of raising OutOfBounds.

**TRANSIENT SCRATCH (the enabling allocator change).** The run's
`ra.denied` set means "not occupied - this fragment may not SPEND it"
(hold a value across a helper call, i.e. pin). A run containing a
MyLang call denies the whole caller-saved pool, which blocked every
`alloc_scratch` ask and silently starved the tier in exactly the shape
it exists for (a load feeding a call). `alloc_scratch(need, prefer,
exclude, transient=true)` now ignores `denied` (never `busy`): a grant
that lives and dies inside ONE op cannot violate any deny reason -
acc_take, which always ignored denied, is the precedent. Anything a
run genuinely occupies (a hoist region's claim, a tag-singleton grant)
is ra.busy and still excludes.

**THE LAUNDERED-TEMP FINDING (test design).** `v = holder[k]` compiles
to `load.elem.v TEMP` + `move v = TEMP`, and MoveV's helper does a
PROPER (registering) copy - so a raw slice copy in the temp is
laundered before any parent write can observe it, and a subscript-shaped
slice test passes even with the decline deleted. The FOREACH form binds
the LOOP VAR as the LoadElemValue dst directly; a parent write while it
is live detaches a registered copy (pre-write view) but leaves a raw
one reading the mutated storage. That shape is the slice-decline
observable in `jit_elemv_native` (u == 41 vs 40, watched). LICM is the
other shape-eater: an invariant `holder[0]` is hoisted to a $licm temp
and the tier never sees the slice - the test's index varies.

**SABOTAGES, all watched failing:** retain deleted -> ASan
heap-use-after-free; release skipped -> LeakSanitizer at exit; jo guard
deleted -> silent wrong answer (the -rt assert); slice decline deleted
-> the foreach divergence (41 != 40, -rt FAIL).

**MEASURED** (callgrind Ir per scale unit, scale3-minus-scale1, `-npc`,
OPT=1 ASSERTS=0 both sides; wall = full-suite interleaved --baseline):

    76_funcval_dispatch   492.0M -> 362.0M  (-26.4%)   wall 0.69x
    62_dict_word_count   1285.8M -> 1019.8M (-20.7%)   wall 0.85x
    47_wordcount          394.2M -> 364.4M  (-7.6%)    wall 0.96x
    46_matrix_mult         20.0M -> 19.1M   (-4.5%)    wall 0.95x

Suite geomean cur/base 0.995x over 89; vdjcmp blast radius 9 of 119
programs changed, 110 byte-identical. REACH: `g_jit_elemv_fast`
(MYLANG_JITSTATS row `elemv_fast`), bumped by the emitted fast path
only. The coverage test is `jit_elemv_native` (tests.cpp); the
`jit_op_nativized` LoadElemValue row accepts the inline counter (the
BinOpV precedent - the helper legitimately starves).

## #97 increment 2 - THE DECLINE LEDGER: "which GUARD did this value
## take?", 2026-08-26

**THE PROBLEM A FAST-PATH COUNTER CANNOT SEE.** `g_jit_elemv_fast`
proves the tier RAN. It says nothing about whether a test's values
ever REACHED a given guard - and a guard no value reaches is a guard
whose test passes with the guard DELETED. Increment 1's first
slice-decline test did exactly that: it passed with the guard removed
while the fast counter read 17.

**THE LEDGER.** Every decline arm gets its own counter
(`g_jit_decline[]`, `ML_FOR_EACH_JIT_DECLINE` in jit.h - one X-macro
so the enum, the name table and the MYLANG_JITSTATS rows cannot
drift). The emitter's half is two functions next to `SlotAddr`:
`decline_jump(e, jumps, cc, why)` records the jump WITH its reason,
`decline_land(e, jumps)` lands them. Under TESTS each distinct reason
gets a landing pad - bump this reason's counter, jump on to the slow
tier; without TESTS every jump patches straight to the landing point.
An unconditional jump over the pads is emitted first, so a caller
never has to reason about fall-through to be correct.
**PROVEN COST-FREE IN A SHIPPING BUILD:** `vdjcmp` of an
`OPT=1 ASSERTS=0` build against the pre-ledger commit is 119/119
byte-identical.

A test then asserts `g_jit_decline[JD_x] > baseline` per guard, which
makes vacuity for THAT guard impossible rather than unlikely. The
report prints a `tier declines (which guard was taken)` section
whenever anything declined, so "the tier ran" and "every value in this
program took guard X" are separate answers.

**IT FOUND TWO VACUOUS CASES IN ITS OWN FIRST TEST, WITHIN A MINUTE:**
 - `elemv_bounds` read **0**. The negative-index case was
   `z = g[runtime(-1)]` - a `runtime()` call in the INDEX splits the
   run, so the op never reached the tier at all. An index computed
   from the LOOP VARIABLE (`g2[i % 2 - 1]`) does: 6 declines, 6 fast.
 - `elemv_base_kind` read **0**. The non-general-storage case used a
   string-literal array - and a plain string LITERAL array is GENERAL
   storage (the value-driven rule: only `split()`/`splitlines()` and a
   hinted `keys()`/`values()` build flat `strs`), so it took the FAST
   path. A flat POD-struct array reaches the guard.
Both are now written up AT the case, so the next reader sees the trap
rather than re-deriving it.

**AND IT ANSWERED A QUESTION NOBODY COULD ASK BEFORE - is a guard
REACHABLE AT ALL?** `elemv_base_not_arr` is taken by nothing: the
codegen emits LoadElemValue only for a statically-proven array base (a
`dyn` base lowers to SubscriptV - verified in `-vd`). It STAYS, and
the reason is now written down instead of assumed: a `.myv` image's
operands are bounded by `verify_chunk` but NOT type-checked (the
ML_UNTRUSTED_CHECK tier-2 case), so a hostile image can point that
slot at anything. The test lists it as a deliberate exemption with
that reason - a non-answer as a row note, never a silent omission.

**THE PATTERN TO REUSE.** Add reasons to the X-macro, call
`decline_jump` instead of `push_back(e.j32(cc))`, `decline_land`
instead of the patch loop, and assert the reasons in the tier's test.
The next tier to use it is the boxed-element STORE twin, whose guards
- read-only array, live-slice detach, hash invalidation, plus the
reference lifecycle - are four separate correctness cliffs.

## #97 increment 3 - THE BOXED-ELEMENT INLINE STORE TIER
## (StoreElemValue), 2026-08-26

**WHAT.** `a[i] = v` into a GENERAL array - increment 1's twin on the
write side - paid the whole helper round trip: `jit_store_elem_value`
-> `vm_subscript_store` -> `TypeArr::subscript(for_write)` ->
`LValue::put` -> `get_value_for_put`, i.e. a virtual subscript
dispatch, an element-LValue round trip with its container
back-pointer, and a type-erased 32-byte assignment. Inline it is the
navigation, the COW guards `put` would have run, and the reference
lifecycle (retain-new / release-old with a cold `jit_release_slot`
arm, the element LValue handed straight to it as `rdi`).

**⛔ ORDER IS SEMANTICS, TWICE OVER:**
 - RETAIN the new value BEFORE releasing the old one, so an aliasing
   store cannot destroy the source it is about to copy. (The source
   slot holds its own retained reference, so an aliasing store's count
   is >= 2 and the ordering is defensive rather than load-bearing -
   written that way, and said so, on purpose.)
 - EVERY GUARD PRECEDES EVERY MUTATION. The interpreter throws on a
   const / read-only / out-of-range store WITHOUT cloning, and the COW
   clone is `intptr`-observable, so a declined store must leave the
   array byte-for-byte untouched.

**THE COW CASES DECLINE RATHER THAN REPLICATE:** a slice base (`put`
clones the whole vector) and an array with LIVE SLICES (`put` detaches
each view in place). With `has_slices == 0` the interpreter's
`use_count > 1 -> clone_aliased_slices` is a no-op over an empty set,
which is why the reference COUNT needs no guard of its own.

**EMIT-TIME declines:** anything but a plain `Op::assign` (a compound
is a read-modify-write through `apply_compound_op`, string
concatenation included - and note the plain form's `aop` is
**`Op::assign`, NOT `Op::invalid`**: this op's `aop` is the Expr14
operator verbatim, which cost the first version its entire reach), a
non-LOCAL base kind, and a literal index (the helper's contract reads
the index from a SLOT).

**⛔ AND ONE LINE DELETED BY A SABOTAGE THAT CAUGHT NOTHING.** The
obvious twin of `get_value_for_put`'s unconditional `invalidate_hash()`
is a byte store, and it was written. Removing it failed NO test - which
sent me to the rule instead of to a better test: `hash_is_cached()` is
`hash_cacheable() && hash_valid`, and `hash_cacheable()` requires kind
in {ints, floats, bools}. This tier is gated on kind == GENERAL, and
representation is fixed at creation (promotion only ever goes flat ->
general), so no reader can consult that flag for this array. Two
instructions per element store, deleted, with the proof at the site.
**A sabotage that catches nothing is evidence about the CODE, not only
about the test** - the two readings are "my test is weak" and "this
line is dead", and the second one has to be ruled out.

**MEASURED** (callgrind Ir per scale unit, scale3-minus-scale1, `-npc`,
OPT=1 ASSERTS=0 both sides; wall = full-suite interleaved --baseline):

    32_str_build_join    273.0M -> 169.8M  (-37.8%)   wall 0.90x
    47_wordcount         364.4M -> 261.2M  (-28.3%)   wall 0.86x
    20_foreach_unpack   1060.2M -> 779.7M  (-26.5%)   wall 0.90x
    46_matrix_mult        19.10M -> 19.06M (-0.2%)    wall 1.03x
    31_str_split_join / 76_funcval_dispatch: Ir EXACTLY flat, emission
    differs (the changed instruction stream reshuffles the peephole)

Suite geomean cur/base 0.995x over 89. vdjcmp blast radius 7 of 119.

**REACH** is `g_jit_storev_fast` (JITSTATS `storev_fast`), and every
one of the tier's ten reachable guards is proven TAKEN by the decline
ledger in `jit_storev_native`. Finding the shapes that reach them was
most of the test work, and three are worth keeping:
 - **`base_slice` fires ONCE, not once per iteration** - the helper's
   store on a slice base runs `clone_internal_vec`, which makes the
   view a standalone non-slice array, so every later store is FAST;
 - **`base_kind` needs a dyn ALIAS of a flat array** (`var a = [1,2,3];
   var dyn d = a;`) - a dyn DESTINATION builds a GENERAL array, so the
   obvious spelling takes the fast path;
 - **`readonly` needs a SLICE OF A CONST in a plain var** - a const
   array reached through a parameter still carries the const flag on
   its slot, so `base_const` shadows `readonly` for that shape.
`storev_elem_const` is unreachable (an array element's LValue is always
constructed non-const; a const CONTAINER is readonly, which an earlier
guard catches) and is carried in the test as a written exemption, for
the same reason `elemv_base_not_arr` is: a `.myv` image's operands are
bounded by `verify_chunk` but never type-checked.

**SABOTAGES, watched failing:** retain deleted -> ASan
heap-use-after-free; release deleted -> LeakSanitizer; the has_slices
guard deleted -> the detach spec fails in `-rt`; the readonly guard
deleted -> a const array becomes writable, `-rt` fails.

## #97 increment 4 - THE INLINE STRUCT-FIELD STORE TIER (StoreMemberV),
## 2026-08-26

**WHAT.** `p.x = i` paid **278.7 Ir**: jit_store_member ->
vm_member_store -> a field-slot resolve, a const/readonly check,
`coerce_struct_field` (which takes its 32-byte EvalValue BY VALUE,
twice) and pod_set - or, for a boxed field, slot_rmw's type-erased
assignment. Callgrind put ~54% of a field-store loop in that chain.

**EVERYTHING THE STORE NEEDS IS A COMPILE-TIME FACT.** The member key
already carries `bake_def` + `bake_slot` (the 64_struct_create fix), so
the emit resolves the FieldDef itself - its KIND and, for a POD layout,
its BYTE OFFSET. The def-identity guard is what makes those facts true
at runtime, the same argument the baked member READ makes.

TWO FORMS, chosen at emit time:
 - **POD**: one store to `[bytes + offset]` (8 bytes int/float, 1 byte
   bool). The value's tag must match the field's kind EXACTLY, which is
   precisely `field_exact_scalar` - the predicate that says
   coerce_struct_field would hand the value straight back. Every real
   conversion (bool -> int, int -> float, `none` into an opt field)
   DECLINES.
 - **BOXED**: the field is `obj.fields[slot]`, a plain LValue with NO
   container, so it is the element tier's reference lifecycle with no
   COW at all: retain-new, release-old (cold arm), copy 24 + the tag.

**NO CLONE ON EITHER PATH, and that is the semantics, not an
omission**: a struct assignment ALIASES (`var q = p; q.x = 77` is
visible through `p`), so vm_member_store writes in place too.

**⛔ THE 32-BIT ADD THAT TRUNCATED A POINTER.** The boxed form's field
offset was first added to the pointer with `add_reg32_imm32` - a 32-BIT
add, which ZEROES the upper half of the register. The field pointer
came out truncated and the store SEGV'd on a small address. Only slot 0
survived (offset 0, add skipped), so ONE field store worked and TWO
crashed - a shape that looks like an interaction bug and is really an
encoding one. Fixed by folding the offset into every DISPLACEMENT (they
are disp32 already, so it costs nothing) and using `lea_base` for the
cold arm's argument. **When an emitted address computation misbehaves,
check the operand SIZE of the arithmetic before the logic.**

**MEASURED** (Ir per scale unit, scale3-minus-scale1, `-npc`, OPT=1
ASSERTS=0 both sides):

    a field-store probe   278.7 -> 70.0 Ir PER STORE   (-74.9%)
    90_struct_field_store 832.4M -> 111.2M  (-86.6%)   wall 0.18x

vdjcmp over the OLD corpus: **119/119 byte-identical** - which is the
finding as much as the win.

**⛔ THE CORPUS HOLE THIS EXPOSED, AND CLOSED.** `member.store`
occurred **ZERO times** in bench/ + samples/, so a field WRITE - as
fundamental an operation as the field read 65_struct_field_sum
measures - was never measured, never differentially exercised over the
lever/rotation matrices, and could not have shown a regression.
`bench/my/90_struct_field_store.my` (+ its CPython twin) is the write
twin of 65, in both shapes. Before this tier MyLang was **SLOWER than
CPython** on it (0.031s vs 0.028s); after, 4.95x faster. That is the
"an oracle's corpus hole is a test hole" lesson again: the number
nobody could see was the bad one.

**GUARD REACH, from the ledger.** Five of seven are reachable and are
asserted TAKEN in `jit_memberv_native`: base_const, readonly,
val_kind, val_ex, val_slice. Two notes worth keeping:
 - **`readonly` needed a readonly OBJECT in a NON-const SLOT**, since
   a const struct through a parameter keeps the const flag and
   base_const shadows it. `func poke(s, v) { var t = s; t.x = v; }` is
   the shape - a struct assignment ALIASES, so `t` is a plain local
   pointing at the same readonly object. Note a const ARRAY element
   does NOT work: `var s = CA[0]` COPIES the struct out, and the copy
   is mutable in all three engines (checked).
 - **`base_not_struct` and `def` are unreachable from compiled code**
   and stay for the untrusted-image reason: the tier is emitted ONLY
   where the member key carries a baked def (a PROVEN struct base), so
   no program can present a non-struct or a different struct there.
   Deleting the def guard was watched and caught nothing, exactly like
   the read tier's identical guard - which is the precedent for
   keeping it.

**SABOTAGE:** the exact-kind guard deleted -> `-rt` FAILS (a real
coercion silently skipped). ⛔ Note the first attempt at that sabotage
left `want` unused, so the BUILD failed with -Werror and `-rt` then ran
a STALE BINARY and passed - a false "not caught". **Check the build's
own exit code before believing a sabotage result.**

**A PRE-EXISTING BUG FOUND IN PASSING (not caused by this tier; `-nj`
shows it):** every VALUE use of a **bool struct field** renders as int
`1`/`0` under the VM and `true`/`false` under the tree-walker - a RULE
2 divergence. Only `p.b == true` agrees, because `==` makes a fresh
bool on both sides. Suspected cause: a bool node is stamped `th == i`,
and the member-read lowering's "bool -> 0/1 int" form writes an INT
into the dst, losing the type. Tracked separately; the test here uses
the comparison spelling, with a note saying why.

## #97 step 2a - THE CAPTURE STORE-TO-LOAD FORWARD (a BYTECODE
## peephole, so both engines get it), 2026-08-26

**WHAT.** A closure that mutates a capture and then USES it re-read the
slot it had just written. `count++; return count` - the whole of
11_closure_counter's callee - compiled to FIVE ops:

    0  load.capture r0, cap[0]
    1  i.bin        r1 = r0 + 1
    2  store.cap    count = r1
    3  load.capture r0, cap[0]     <- re-reads what op 2 just wrote
    4  return.v     r0

Op 3 is not cheap. Reaching a capture is `ctx -> captures -> data()` -
THREE CHAINED LOADS, re-derived at every access, since nothing caches
the base - then the type guards and a 24-byte boxed copy: about TWELVE
emitted instructions for a value already sitting in a frame slot.

**THE RULE.** `store.cap k = v ; load.cap d, k` -> the load reads `v`
instead. SOUND because a plain-assign StoreCaptureV is exactly
`lv.put(RValue(frame[v]))`: no coercion (unlike a declared-type slot
store), and a capture LValue has no container, so no COW can intervene.
The ops are ADJACENT, so nothing can write `v` between them.

**⛔ THE `aop` GATE IS THE SOUNDNESS CONDITION.** A COMPOUND store
(`acc += x`) writes num_binop's RESULT, which is NOT what slot `v`
holds - forwarding `v` there returns the ADDEND. Watched failing:
removing that one condition turns `compound 101 103 106` into
`compound 1 2 3` in tests/functional/21_capture_forward.my.

**⛔ AND THE TWO-OP FORM WAS NOT WORTH MUCH - THE THREE-OP WINDOW IS.**
Rewriting the load into a `MoveV` alone measured only **-1.56%**: a
boxed MoveV has its own type guards and 24-byte copy, so it traded ~12
emitted instructions for ~9. Extending the match to
`store.cap; load.cap; return.v` - pointing the RETURN straight at `v` -
removes both, and the body drops to four ops with no capture read at
all after the store. **-10.42%.** The lesson generalises: replacing an
expensive op with a cheaper one is worth a fraction of DELETING it, and
the peephole's pair window hid the difference until the third op was
looked at.

The consumed load is neutralised as a `Jump` to the next pc, the idiom
the LoadElemInt/JumpUnlessTrueV fusion already uses - the peephole
rewrites IN PLACE and cannot erase, because pcs are jump targets.

**MEASURED** (Ir per scale unit, scale3-minus-scale1, `-npc`, OPT=1
ASSERTS=0; wall from an interleaved --baseline):

    11_closure_counter  192.0M -> 172.0M  (-10.42%)   wall 0.88x
    63_closures         346.2M -> 338.2M  (-2.31%)    wall 0.99x

**THIS ONE MOVED THE WALL CLOCK, AND THE STEP-1 WORK DID NOT** - worth
recording side by side, because the difference is the whole lesson of
the guard-elision family. Step 1 removed 17 `movabs reg, imm64` per
call: register writes with no memory access, which a wide OOO core
absorbs (Ir -8.1%, wall 1.00x). This removes three chained LOADS and a
24-byte copy - real memory traffic - and 10% of the instructions became
12% of the time.

It is a BYTECODE peephole, so the interpreter gets it too; and being
engine-independent it is covered by corpus_diff against the
tree-walker, which never had the rule.

## #97 step 2b - THE POST-CALL vframe RESTORE, BAKED FOR MAIN,
## 2026-08-26

Every emitted return repoints the activation's vframe view at the
caller's window and nslots. A non-main caller stored an IMMEDIATE
(`caller_total`, from its descriptor); a MAIN caller read its nslots
back through TWO CHAINED LOADS - `act->top_rec`, then `rec->nslots` -
purely because `caller_total` is derived from a DESCRIPTOR and main has
none.

But main's window size is not a runtime fact. The root window is pushed
in exactly one place with `nslots = root_slot_count + root.n_temps`
(vm.cpp), and the chunk being emitted IS main's, so the same immediate
store the other arm already used is correct. A POINTER CHASE off the
return path is worth more than its instruction count suggests: on a
RECORD-LESS return the record's cache line is otherwise never touched.

    11_closure_counter  172.0M -> 170.0M  (-1.16%)
    63_closures         338.2M -> 336.2M  (-0.59%)
    76_funcval_dispatch 348.0M -> 346.0M  (-0.57%)
    09_fib_recursive          flat (its caller is a function, so it
                              already took the baked arm)

**NETS - and this is the change that needed the frame-shaped ones**, a
wrong nslots corrupts the VM's view of the caller's window rather than
producing a wrong value: `norec_enum.py --depth 3` (1920 engine runs
over the enumerated frame-kind space, 9 of 25 sampled programs making
record-less pushes) and `norec_sweep.py` (the forced reconstruction at
every call event) both agree, plus -rt 1961/1961 and corpus_diff 29/29.

## #97 step 3 - THE CLOSURE STORE IS INLINE, THE CONSTRUCTION IS NOT
## (63_closures -11.2% Ir, 0.91x wall), 2026-08-26

**WHAT.** `jit_make_closure` built the closure AND stored it, and the
STORE was the expensive half: `frame->at(dst).put(EvalValue(..))`
reaches `EvalValue::operator=(EvalValue&&)`, which destroys the old
value and move-constructs the new one through the TYPE-ERASED ops
table - TWO INDIRECT CALLS plus their machinery. Measured on
63_closures that pair is ~125 Ir per closure, against ~110 for building
the object.

Emitted code knows two things C++ cannot: the new value's type is
t_func, and the pointer can arrive with its count ALREADY at 1. So
`jit_make_closure_ptr` CONSTRUCTS ONLY and transfers ownership (the
count is set by hand rather than by an intrusive_ptr that would release
it on the way out), returning null after conveying a throw - the
capture snapshot can raise UnboundSymbolEx. The store is then the
element tier's shape: release the old value (dec, cold
`jit_release_slot` arm for destruction / a slice / an exception
object), then two stores.

    63_closures         336.2M -> 298.6M  (-11.18%)   wall 0.91x
    11_closure_counter / 76_funcval_dispatch: flat (no closure CREATION
    in their loops - 11 calls one closure a million times)

63_closures: **18.19x -> 16.23x** vs C++.

**⛔ THE TESTING LESSON, AND IT IS THE BIGGER HALF OF THIS ENTRY.**

**(1) POOLING BLUNTS LeakSanitizer.** FuncObject carries
ML_POOL_NEW_DELETE, so a leaked closure is memory the POOL already
owns. Deleting the release arm and running ONE 300-closure program is
SILENT under ASan+LSan - the leak fits inside a chunk LSan sees as
reachable. Only at SUITE volume does it surface, at exit, as
"179560 byte(s) leaked in 1349 allocation(s)" with no hint which tier
is at fault. A TESTS-only live-object count (`g_live_funcobjs`) fails
immediately, at any volume, and NAMES the tier. **The same blunting
applies to every ML_POOL_NEW_DELETE type.**

**(2) THREE SHAPES BEFORE THE TEST CAUGHT ITS OWN BUG** - the
vacuous-test trap, twice in a row, both times passing GREEN with the
whole release arm deleted:
 - at the TOP LEVEL, `var f = ...` is a GLOBAL, so MakeClosureV targets
   a TEMP and a StoreGlobalV moves the value out: the global store owns
   the lifetime and this tier frees nothing;
 - `f = mk(i)` is a CALL - the closure is built inside `mk`, into ITS
   temp, and RETURNED, so the dst is again not `f`.
What targets a live slot is a closure LITERAL assigned to a ref-listed
LOCAL inside a function, which `-vd` shows as
`make.closure f = closure_defs[0]` instead of a temp. **CHECK THE OP
THE TEST PRODUCES, NOT THE SOURCE SHAPE** - this is the same rule the
vacuous-test list already states, met twice in one test.

The DANGEROUS direction needs no such help: dropping the ownership
transfer in `jit_make_closure_ptr` is an ASan heap-use-after-free on
the first run (watched).

NETS: -rt 1962/1962 (dbg ASan+UBSan and RECYCLE=1); corpus_diff plain /
--levers / --xrot / --cold 30/30; disasmcheck vs objdump zero
disagreements; tests/functional/22_closure_store.my for the ownership
shapes (overwrite, last-ref, shared, trivial-to-reference,
reference-to-reference, independence, array/field destinations).

## #97 step 4 - THE BAKED CALLEE: the emitter NAMES the callee
## at compile time (2026-08-27)

**THE PROBLEM.** The inline call push spends ~14 instructions and ~9
loads re-establishing, on every call, five facts that never change: the
callee's arity, its `fast_bind` flag, that its chunk exists and starts
native, that its frame carries no per-frame side state, and how big its
window is. The monomorphic callee CACHE (G1) already noticed this - a
hit re-establishes all five with one pointer compare - but a cache is
still a runtime data structure: a baked address, a load, a compare, and
then the derived values loaded back out of the descriptor and the chunk.

**THE OBSERVATION.** For a call to a WRITE-ONCE global slot the emitter
does not need a cache at all: `JitCtx::slot_desc` maps the slot to the
`FuncDescriptor` declared there and `slot_reassigned` says no assignment
targets it. That is the SAME gate `callv_native_ok` (#55) has used since
2026, applied one layer down - to the PUSH rather than to the call.
`jit_baked_callee()` is that gate; everything the five checks ask
becomes a compile-time constant, and one identity compare is what
survives to run time.

**WHAT IS BAKED**, and what each replaced:

    the five descriptor gates      13 instructions   -> 0
    the callee-cache probe          4 (movabs/cmp/je/cck load) -> 3
    the window size                 4 (2 sign-extended loads + add)
                                                     -> 1 (an imm32)
    the record-less FORK            2 (a byte load + a branch) -> 0
    a proven-scalar ARGUMENT's
      reference test                4 per argument   -> 0

    09_fib_recursive     152.95M -> 147.95M per scale unit  (-3.27%)
    63_closures                                             (-2.61%)
    11_closure_counter                                      (-2.94%)
    76_funcval_dispatch                                     (-1.45%)

63 and 11 call CLOSURES, whose callee is a runtime value - they get
none of the bake and their numbers come from step 4's arena work below.

**THE SCALAR ARGUMENT BIND, and why `ref_slots` could not answer it.**
The per-argument copy loads the source's `Type *`, dereferences its `t`
field and branches to a reference arm - four instructions to decide
something codegen already knows. The obvious source of truth is the
CALLER's `ref_slots`, and it is useless here: an argument temp is
written by a staging `MoveV`, `op_writes_scalar` does not list MoveV, so
`compute_ref_slots` marks EVERY argument temp as reference-carrying.
(Measured: the elision fired 0 times corpus-wide on that predicate.)
The answer is the CALLEE's `ParamDesc::binds_scalar()` - the same
predicate codegen uses to leave a parameter OUT of the callee's
`ref_slots`, so the bind and the release scan cannot disagree.

**⛔ TWO OF THE CALLEE'S PROPERTIES ARE WRITTEN BY ITS OWN JIT PASS.**
`sync_entry_off` and `norec_ok` are both set at the END of
`jit_compile_chunk` for the callee's chunk, so a SELF-RECURSIVE call -
fib -> fib, the flagship shape - reads them UNSET. The first version
gated the whole bake on `sync_entry_off >= 0` and 09_fib_recursive's
reach was **4 of 555,823 calls**; `norec_ok` unset reads as FALSE, which
would have silently disabled the record-less tier at exactly that site.
The audit-table stage trap in its "a value computed later" shape.

**⛔ AND "READ IT IF IT HAPPENS TO BE SET" IS WORSE THAN NOT READING
IT**, because it makes the EMITTED CODE DEPEND ON COMPILATION ORDER -
and Pass B walks a POINTER-keyed map, whose order is not stable across
runs. `-vdj` reproducibility (two runs, two separately-linked binaries,
byte-identical text) is what `scripts/vdjcmp.sh` IS. So the elision is
allowed for exactly one caller, on a structural argument: **MAIN is
compiled LAST**, after every function body (`vm_precompile_all`'s Pass B
and `vm_jit_loaded_image`), so every callee it can name is already
placed. Every other caller keeps the two runtime tests. `bake_final` is
that distinction; a null `g_cur_caller_desc` IS main.

**MAIN NEEDED A `JitCtx` AT ALL, WHICH IT DID NOT HAVE.** #55 gave main
none, on the correct reasoning that main has no stable descriptor and so
cannot make a native DIRECT call (`callv_native_ok` still declines on
`!jc->caller_desc`). But the MAP is what the bake needs, and main is
where the corpus's call loops live - every headline bench's outer loop.
Main's jit moved inside `vm_precompile_all`, after Pass B, with the same
map and a null `caller_desc`; `vm_jit_loaded_image` does the same, so a
loaded image's main emits what a fresh compile's does.

**⛔ THE WRITE-ONCE GATE IS A PROFITABILITY GATE, NOT A SOUNDNESS ONE -
AND THAT IS WHY THE FORCE LEVER EXISTS.** Soundness comes from the
emitted identity compare: a reassigned slot holding a different function
fails it and declines. So removing the WRITE-ONCE gate cannot produce a
wrong answer - it produces a site that can never pass its compare, i.e.
every call to the C++ tier with the two-entry cache never consulted
(watched: the test's second case reports `callee_cache` 0).

And removing the COMPARE, under our own compilation, changes nothing any
net can see: a write-once slot can hold only its own declaration's
FuncObject or `none`, and `none` fails the type test. Watched with the
compare deleted: **-rt 1963/1963 and corpus_diff 31/31, both green.** It
is still the guard an untrusted `.myv` image needs, whose slot ->
descriptor map is resolved BY NAME out of the file.
**`MYLANG_JIT_FORCE=bakecallee` lifts the profitability gate and leaves
the soundness one** - exactly what the FORCE half of a lever pair is
for - which makes the compare reachable by a test. With two callees of
different frame sizes, deleting the compare then aborts in `Frame::at`
(rc 134, watched).

**SABOTAGE LEDGER** (each reintroduced, built, and watched failing):

    the write-once gate      -> -rt names it ("the declined site did not
                                reach the cache tier either (0)")
    the identity compare     -> abort in Frame::at under FORCE (rc 134)
    the baked window size +1 -> abort in Frame::at (rc 134)
    every argument claimed
      scalar                 -> -rt fails AND corpus_diff fails

NETS: -rt 1963/1963; corpus_diff plain / --levers / --xrot / --cold /
--nolowmem; disasmcheck vs objdump zero disagreements;
`tests/functional/23_baked_callee.my` (main and nested callers,
self-recursion, a reassigned slot, a reference argument, a mixed
signature, a decl re-bound per loop iteration, floats, and a throwing
callee unwinding through the record-less arm).

## #97 step 4a - THE ARENA RESIDUALS: three encodings the low
## arena had already paid for

Step 1 moved the JIT's globals into the low-address arena so they could
be named by an `imm32`. Three sites on the call path never collected:

 - **the callee TYPE test** was `movabs rax, t_func` + a compare. The
   Type singletons ARE arena-allocated (`ml_lowmem_new<TypeFunc>()`), so
   `cmp qword [slot+24], <t_func>` is ONE instruction. The missing piece
   was an ENCODER: the tag seam had a register-compare
   (`cmp_reg_tag`) and a store (`store_type_tag`) but no MEMORY compare,
   so nine call sites open-coded the pair. `Emitter::cmp_mem_tag` is the
   third member, and like its siblings it takes the tag as an ARGUMENT -
   the rule that exists because a wrapper naming its operand in the
   METHOD is invisible to an audit that greps for the operand;
 - **the residue relay** pushed and popped `g_jit_residue_caps` through
   a `movabs`-materialised address, twice per call.
   `Emitter::push_abs32` (`FF /6` through the no-base SIB form) and the
   existing `store_abs32` make each one instruction;
 - **nine `movabs X, &global; mov Y, [X]` pairs** - five of them on the
   RETURN path, which every call pays - became `load_global`, the seam
   that already picks the one-instruction form.

Measured **-3 instructions per call**, exactly as predicted, on all four
call benches (63_closures / 11_closure_counter / 76_funcval_dispatch
each moved by precisely 3 Ir x their call count).

**The generalisation worth keeping: an infrastructure change pays only
where a SEAM exists to collect it.** The arena was in place for ten
days; these three sites kept their two-instruction forms because no
encoder offered the one-instruction one, and nothing in the tree could
notice.

## #97 step 5 - `ref_slots` LEARNS WHAT A `MoveV` ACTUALLY WRITES
## (2026-08-27)

**MEASURED FIRST, for once.** `scripts/jitprofile.py` (built in the
same session, because nothing could answer this) put a 1-argument call
to a 3-op function at **142 Ir**, split exactly:

    the ARGUMENT round trip     28   (17 staging move + 11 bind copy)
    ACTIVATION bookkeeping      71   (segment fit, window advance, the
                                      record gate, vframe + captures
                                      repoints, residue push/pop, and a
                                      26-instruction residue RETURN arm)
    the irreducible call        24
    the callee's entry + body   15

and the marginal cost of one more argument at **28 Ir**, measured
independently by varying the arity. The `#162` recognizer's own comment
estimated the staging move at "~8 instrs". It is 17.

**WHY 17.** `MoveV`'s emit tests BOTH sides for reference-ness before
the raw copy - four instructions each - and only the DEST test was
gated on `ref_slots`. The source test was unconditional, though it asks
exactly the question the chunk-wide invariant already answers. Gating
it: **-4 per staged argument**, and it needed nothing but the gate its
sibling already had.

**AND WHY THE DEST TEST FIRED ON EVERY ARGUMENT IN THE CORPUS.**
`op_writes_scalar` cannot list `MoveV` - it copies whatever the source
holds, which is the op's whole point - so `compute_ref_slots`'
conservative fallback marked its dst. A call's arguments are STAGED by
MoveV, so **every argument temp in every program was
reference-carrying**, for an `int` argument, forever. The cost was paid
three times per call: the staging move's dest check, the bind copy's
per-argument source test, and the frame pop's release scan. It also made
`#162`'s "only a ref-listed slot is ever fused" gate look like it
separated shapes it did not.

The rule is one line of dataflow - `is_ref[dst] |= is_ref[src]` for a
move, to a fixpoint, since a loop body's move can precede its source's
own marking - and conservative everywhere else.

    63_closures          -1.11%   (the A/B: the move rule alone)
    76_funcval_dispatch   0.00%
    09_fib_recursive      0.00%

**Small, and honestly so.** It is kept for three reasons beyond the
number: it only ever SHRINKS a set that was wrong in the expensive
direction; it shortens the return-path release scan everywhere; and it
forced the ordering fix below, which was a latent trap for any future
refinement.

**⛔ THE ORDERING FIX, AND `jit_ret_audit` CAUGHT IT ON THE FIRST RUN.**
A reference PARAMETER's slot is written by the BIND, which is no
instruction, so `visit_use_def` cannot see it - `compile_func_body`
unioned the non-scalar params into `ref_slots` AFTER `compute_ref_slots`
returned. That was safe while every write was marked INDEPENDENTLY, and
became wrong the instant a MoveV's answer DEPENDED on its source: `move
t = param` then propagated from a source the pass believed trivial. The
fix is not to redo the union later but to make the INPUT complete -
`codegen_chunk` takes `ref_seeds` and the params go in before the
fixpoint.

**The generalisation: a table's inputs must be complete before any rule
in it becomes RELATIONAL.** An entirely-independent marking pass
tolerates a late union; the first rule that reads one entry to decide
another does not. Same family as the audit-table stage trap, one level
in.

**SABOTAGE LEDGER**, each reintroduced, built and watched failing:

    the seeds merged AFTER (the old order) -> jit_ret_audit, rc 134
    the move propagation deleted           -> jit_ret_audit, rc 134
    the MoveV DEST check dropped           -> -rt AND corpus_diff fail
    the SOURCE check never emitted         -> -rt AND corpus_diff fail

**AND `LoadConstV`, THE SAME SHAPE ONE STEP OUT (built the same day).**
It is absent from `op_writes_scalar` because a constant CAN be a string
or an array - but WHICH constant is a compile-time fact and
`compute_ref_slots` holds the pool. Without it a loop counter
initialised by `load i, 0` is reference-carrying for the whole chunk,
and the MoveV rule above then faithfully propagates that into every
argument staged from it: **a refinement is only as good as the facts
under it.**

Its measured effect on the four call benches is **0.00%**, and it is
kept anyway on evidence rather than on taste: the restored
`vdjcmp.sh` says it changes the EMITTED CODE of **9 of 124** corpus
programs, so it has real reach - just not in the shapes those four
benches use, where temp REUSE (an argument temp shared with a string
argument to `print`) marks the slot for a genuine reason. A rule with
reach and no measurable cost, under the same two audits, stays.

NETS: -rt 1964/1964 on dbg(ASan+UBSan), RECYCLE=1, rel-hard(VM_HARDENING)
and clang; corpus_diff plain/--levers/--xrot/--cold/--nolowmem;
disasmcheck vs objdump zero disagreements; driver_checks; norec_enum
--depth 3. The soundness oracle for a SHRUNK `ref_slots` is
`jit_ret_audit` plus the VM_HARDENING every-slot-trivial audit at
`pop_window`, and both demonstrably fire.

## #97 step 6 - THE FRAMELESS GATE, MEASURED BEFORE IT WAS BUILT
## (2026-08-27) - INERT: it decides nothing yet

The measured budget says **71 of a 142-Ir call is ACTIVATION
BOOKKEEPING** - the segment fit test, the window advance, the record or
its record-less fork, the residue push/pop, the vframe and captures
repoints, and a 26-instruction residue RETURN arm. All of it exists so
the interpreter can resume mid-call and an unwind can rebuild frames. A
callee needing none of it could take its window off the NATIVE STACK
(`sub rsp, N*48`) and return in five instructions: projected **~42 Ir
against 142**.

**⛔ THE GATE WAS MEASURED FIRST, AND THAT DECISION PAID FOR ITSELF
THREE TIMES OVER.** The design has an obvious way to be worthless -
09_fib_recursive is SELF-RECURSIVE and 63/11/76 call CLOSURES, so a
leaf-only, named-callee tier could serve exactly zero of the four
benches it exists for. That is the vacuous-test trap one level up: not a
test that proves nothing, a TIER that serves nobody. `MYLANG_FRAMELESS_WHY=1`
reports the verdict per chunk, and each wrong gate died in one run:

    gate version              corpus chunks admitted (of ~253)
    `native_leaf`                    - reach 0, reasons meaningless
    + terminal ReturnV only          18   (Halt bodies all rejected)
    + `ref_slots.empty()`            20   (that clause alone: -209)
    + `ref_slots <= RET_REF_GUARD_MAX`  83   (33%)

Each rejected clause was wrong for a *reason*, not by an off-by-one:

 - **`native_leaf` is #55's gate and means something else.** It requires
   `op_never_exits` of EVERY op because a #55 direct caller IGNORES the
   fragment's return, so a conveying throw would be dropped. A frameless
   callee does not ignore the return - its throw conveys through the
   postexit exactly as a record-less frame's does.
 - **A `Halt`-terminated body is fine.** `emit_ret_native(e, ck, -1)`
   already lowers Halt as "return none". Requiring ReturnV rejected 131
   of 256 chunks - every void function and every `main`.
 - **`ref_slots.empty()` was a wall, not a gate**, and its stated reason
   ("nothing to release at the return, nothing in the window an unwinder
   must see") was false on both halves: the window is addressable
   through rbx wherever it lives, so the release scan runs exactly as
   `emit_ret_native`'s does, and the unwinder reads the window through
   the same rbx, never through the segment. What the bound is for is the
   SCAN'S COST, so it is the norec tier's own `RET_REF_GUARD_MAX`.

**⛔ AND THE ANSWER THE REACH NUMBER GIVES IS STILL NO** - which is the
whole point of asking before building. Per CALL SITE, with the callee
resolvable at compile time:

    09_fib_recursive     0 sites   (fib calls itself: not a leaf)
    63_closures          2 sites   (of 5 calls per iteration)
    11_closure_counter   1 site    (cold - the hot call is a CLOSURE)
    76_funcval_dispatch  0 sites   (the callee is a func VALUE)

**The blocker is not the callee's BODY, it is that the hot callees are
reached through a VALUE or are RECURSIVE**, so the caller cannot name
them at compile time or they are not leaves. A leaf-only,
baked-callee-only frameless tier cannot deliver the requirement. What it
would take is written in task #97.

**⛔ THE BACKTRACE IS NOT THE PROBLEM, AND THIS IS WORTH RECORDING
BECAUSE IT WAS THE FIRST WORRY.** A frame is named from TWO sources that
are already disjoint:

 - the PHYSICAL frames come from the **rbp chain** - `frag_entry` pushes
   rbp then rbx FIRST (a documented load-bearing order), so `[rbp+8]` is
   the return address, `[rbp]` the caller's rbp and `[rbp-8]` the
   caller's WINDOW. `norec_walk_chain` already descends it, and names
   each frame by the site of the frame above (`desc(frame below) =
   site(this frame).caller_desc`);
 - the VIRTUAL frames from INLINING come from pc-keyed side tables baked
   at compile time - the callee's own `inline_frames` at the raise pc
   (`vm_flush_inline`) and the CALL SITE's `NorecSite::inline_chain` +
   `inline_pool` (`vm_jit_stamp_call_site`).

**Neither consults rbp, and a frameless callee changes neither.** It
keeps the same prologue, so the physical chain is byte-identical; and
the inlined-at chain of a call op belongs to the CALLER, whose frame the
tier does not touch. `jit_norec_postexit` is already ~the frameless
postexit: the four state restores it performs (the segment watermark,
`ctx.captures`, the vframe repoint, the release scan) become no-ops or
unchanged, and `ex->backtrace.emplace_back(d, Loc())` - the line that
names the frame - is untouched.

WHAT LANDED: `Chunk::frameless_ok` + `jit_chunk_frameless_ok`, derived
at codegen beside `native_leaf`; `MYLANG_FRAMELESS_WHY=1`, the reach
report; two TESTS counters (`frameless_chunks`, `frameless_calls`); and
`jit_frameless_gate`, which pins each clause. Nothing reads the flag to
decide emission.

**SABOTAGE LEDGER**, and the leaf clause needed TWO corrections before
it was falsifiable at all:

    the plain_frame clause deleted -> -rt names the try-region case
    the leaf clause deleted        -> -rt names the builtin-call case
    ...with a MyLang call instead  -> GREEN. `CallV` is not
        `jit_op_eligible` (calls join a run via `op_run_eligible`), so
        the eligibility test rejects first and the leaf clause decides
        nothing. `CallBuiltinV` IS eligible, so only a BUILTIN caller
        makes the clause load-bearing.
    ...with `len()` as that builtin -> the body QUALIFIED: lever 4b
        fuses `len()` into the native `ArrLen` and the call disappears.
        `max()` survives as `call.blt.v`.

## #111 - THE PROVEN-SCALAR CAPTURE: 40 emitted instructions for one
## `add` (2026-08-27)

**FOUND BY `scripts/jitprofile.py`**, built the same day, while sizing
#97's frameless tier - and it redirected that work. The instrument's
first real use paid for it.

    func mk(start) => func [start] { start++; return start; };

`11_closure_counter` calls that closure a million times. Its fragment
cost **83 Ir per call**, split:

    entry                    5
    THE CAPTURE READ        20    ctx -> captures -> data() (3 loads),
                                  then a boxed 32-byte EvalValue copy
                                  with a REFERENCE CHECK on each side
    the actual `+1`          4
    THE CAPTURE WRITE       20    the same chain, walked again from
                                  scratch, the same copy, the same checks
    the return arm         ~31

**40 of 83 - and 24% of the whole program - for one integer increment.**

**WHY: THERE IS NO TYPED CAPTURE OP.** `LoadElemInt`,
`LoadStructFieldInt`, `DictLoadInt` all exist; a capture has only the
boxed `LoadCaptureV` / `StoreCaptureV`, so a proven-`int` capture takes
the general path that must assume any value. The gap was KNOWN and
written down - `try_capture_leaf`'s own comment says "the dedicated
typed load ... is a separate, later question - the arithmetic is where
the cost was". The arithmetic was fixed then; the load was not.

**THE FIX NEEDS NO NEW OPCODE AND NO FORMAT BUMP.** `Instr::cap_scalar`
rides bit 0x40 of the already-serialized `opflags`, per-OPCODE like
`struct_checked` on bit 7 (the element and capture families do not
overlap, and `set_a`/`set_b` mask only 0x07/0x38). It is set at the two
sites where the inferencer's `TypeHint` is in hand - `try_capture_leaf`
and `emit_typed_capture_update` - and it is a HINT: the VM ignores it
entirely, so only a WRONGLY SET flag is unsound.

Three consumers, and the third is the one that compounds:

 - the emitted READ drops the source reference check and copies 8
   payload bytes instead of 24;
 - the emitted WRITE drops the source gate AND the capture's own
   current-value check - a proven-scalar variable never holds a
   reference, so there is nothing to release;
 - **`compute_ref_slots` learns it too.** A `cap_scalar` LoadCaptureV
   writes a proven scalar, so its dst leaves `ref_slots` - which
   removes the read's *destination* check as well and shortens the
   return path's release scan. That is the THIRD instance in one day of
   the same rule (MoveV, LoadConstV, and now this): **a table entry that
   says "this op writes anything" is right about the OPCODE and wrong
   about the INSTRUCTION.**

And with every guard elided, **nothing jumps to the helper** - so
neither the join `jmp` nor the helper body is emitted at all. That is
one executed instruction per access and a page of unreachable bytes out
of the fragment's I-cache footprint.

    the closure's fragment    83 -> 53 Ir per call   (-36%)
    11_closure_counter                    -20.6%   (was -2.9%)
    78_typed_param_call                   -12.4%   (was ~0)
    63_closures                            -9.9%   (was -4.8%)
    76_funcval_dispatch                    -2.6%   (unchanged - no
                                                    capture in its loop)

`78_typed_param_call` was not a target and moved 12% - `func [base]
(int k) { return base + k; }` is the same shape.

**SABOTAGE LEDGER**, each reintroduced, built and watched failing:

    the flag set on EVERY capture load
      (so a REFERENCE capture claims scalar) -> -rt AND corpus_diff fail,
      18_store_src_gate printing `<none> 399` where it prints `599`
    the JIT ignoring the flag on the READ
      (cscal forced true)                    -> -rt AND corpus_diff fail

**WHAT IS LEFT, MEASURED NOT GUESSED.** The `ctx -> captures -> data()`
chain is still walked from scratch at EACH access - 3 instructions, 6
per call for this closure - though it is loop-invariant within a
fragment. Hoisting it needs a register held across the run (the C1
shape) or a cached arena cell maintained at every site that writes
`ctx->captures`; the second is 4 instructions for a miss that would read
the WRONG closure's captures, so it is not obviously worth it. Recorded,
not built. **BUILT the next day as #112, by the FIRST route** - a
callee-saved register held for the run. The arena-cell variant was
rejected for the reason recorded here plus a stronger one: it makes
every writer of `ctx->captures` responsible for a derived global, which
is the "an && over a family is a table" shape with a use-after-free as
its failure mode.

NETS: -rt 1965/1965 on dbg(ASan+UBSan), RECYCLE=1, rel-hard
(VM_HARDENING) and clang; corpus_diff plain/--levers/--xrot/--cold/
--nolowmem; disasmcheck vs objdump; driver_checks; vdjcmp self-test;
norec_enum --depth 3; `tests/functional/24_capture_scalar.my` for the
boundary (int / float / bool take it; an array, a string, a `dyn` that
alternates int and string, and two closures over one factory call must
not, and are run under RECYCLE=1 + ASan where a torn handle or a missed
release is a use-after-free).

## #112 - THE CAPTURE BASE IS WALKED ONCE PER RUN, NOT PER ACCESS
## (2026-08-27)

**THE PROFILER FOUND IT, AND NOTHING ELSE COULD HAVE.**
`scripts/jitprofile.py` on 11_closure_counter printed the closure body
in address order, and six of its fifty-three instructions were the same
three dependent loads, emitted twice:

    +12  mov rax, [<addr>]      +91   mov rax, [<addr>]
    +20  mov rax, [rax+0x78]    +99   mov rax, [rax+0x78]
    +27  mov r9,  [rax+0x0]     +106  mov r9,  [rax+0x0]
    +34  mov rax, [r9+0x18]     +113  mov rax, s1.type
    +41  mov rcx, [r9+0x0]      +120  mov rcx, s1
    +48  mov s0, rcx            +127  mov [r9+0x0], rcx
    +55  mov s0.type, rax       +134  mov [r9+0x18], rax

`ctx -> ctx->captures -> data()`, once for the READ of `start` and again
for the WRITE. Nothing between them is a call - a slot move and an
`add`. `-vdj` shows the same bytes but says nothing about which of them
run; the whole point of the join is that the answer is per-INSTRUCTION.

**THE SHAPE, and why it is not a peephole.** The two accesses are not
adjacent and there is no window in which "the base is still in r9" is
locally provable - proving it means knowing which registers every
intervening emit arm clobbers, which is the audit that failed for r9 in
the pin pool. So the value is HOISTED instead: a run making TWO OR MORE
inline capture accesses claims a register for the whole run, walks the
chain once at the run head, and every access reads it. That is C4b's
pinned float literals (`Emitter::flits`) applied to a pointer.

**THE REGISTER IS CALLEE-SAVED, AND THAT IS LOAD-BEARING.** It is taken
from whatever `CACHE_REGS` ({r12..r15}) the pins, the C2b hoist pair and
trans mode's abstract registers left - claimed LAST, so it can only ever
spend a register nothing above it wanted. Callee-saved means a helper
call preserves the REGISTER, so only its VALUE is refreshed, at one
place (`emit_call_epilogue`). A caller-saved register would need
refreshing after every call the emitter makes, and which emitted
sequences call is not scannable - the flits note records a float store
to a ref-listed dst calling `jit_put_float` from an arm no opcode scan
sees.

The walk goes THROUGH the destination register (`mov cb, [ctx]; mov cb,
[cb+0x78]; mov cb, [cb+0]`), needing no scratch - which is what lets it
run at the epilogue, where rax carries the helper's status that every
call site tests immediately after.

**⛔ IT IS A BASE REGISTER, NOT A VALUE REGISTER - SO r12 IS EXCLUDED,
AND `load_base0` HAD TO LEARN r13.** Both halves are the same fact
about ModRM, and both were caught by an existing instrument on the first
run rather than shipped:

 - `(r12 & 7) == 4` means "a SIB byte follows", which `load_base` does
   not write. `base_needs_sib`'s ML_CHECK fired immediately - and its
   own comment had PREDICTED this moment: *"nothing passes r12/r13 as a
   base today, which is precisely why it was silent; it stops being
   silent the moment a base register comes from an ALLOCATOR rather
   than a literal."* r12 is skipped by the claim.
 - `(r13 & 7) == 5` at mod=00 means "disp32, no base". Every access
   addresses off the base with a disp32 (mod=10), which is legal for
   r13 - but `load_global`'s OFF-ARENA fallback ends in `load_base0`,
   the mod=00 form, so `load_base0(r13, r13)` aborted. **On the
   low-address arena that line is never reached** (`load_abs32`
   returns first), so it is invisible to every default build: the
   `--nolowmem` lane found it, four programs, one lane.
   FIXED IN THE EMITTER, not dodged in the caller: `load_base0`
   delegates to `emit_modrm_disp`, the shared tail that already picks
   mod=01 disp8=0 for rm==5. Byte-identical for every existing caller
   (none passes rsp/r12/rbp/r13), and one fewer place the register-class
   work will trip over.

**MEASURED** (callgrind Ir, `OPT=1 ASSERTS=0` both sides, the
scale3-minus-scale1 delta so compile time is excluded):

    11_closure_counter   270,000,027 -> 264,000,027   -2.22%
    63_closures          538,400,000 -> 536,000,000   -0.45%
    78_typed_param_call        flat (+850 whole-program, both scales)
    76_funcval_dispatch        flat (+438 whole-program, both scales)

Both deltas are EXACTLY three instructions times the run's own capbase
counter (3 x 1,000,000 and 3 x 400,000) - the arithmetic the emitted
counter predicts, which is the check that the reach and the saving are
the same event. The closure fragment is 53 -> 50 instructions per call,
and the callee-saved `push` is FREE: it replaced the `sub rsp, 8`
alignment pad `frag_entry` was emitting anyway.

**WALL CLOCK** (one interleaved `--baseline` A/B, `OPT=1 ASSERTS=0`
both sides): 11_closure_counter **0.99x**, 63_closures **0.99x**,
78_typed_param_call 0.98x, suite geomean **cur/base 0.997x over 90**.
Small, and honestly so: three instructions out of fifty in a fragment
whose remaining cost is the call protocol's memory traffic. It is on
the right side of the noise band on both closure benches and nothing
regressed. Read `cur/base`, not the my/cpp column - the same run put
63_closures at 16.82x my/cpp where the previous session's run said
14.65x, which is the comparison denominator drifting, not this change.

**BLAST RADIUS**: `vdjcmp` says 119 of 125 corpus programs are
BYTE-IDENTICAL. The six that change are exactly the six the pre-build
census predicted - 11_closure_counter, 63_closures,
06_calls_closures, 18_store_src_gate, 21_capture_forward,
24_capture_scalar. `12_compound_bitops` has two capture ops and
correctly DECLINES: one is a COMPOUND store, which calls
`jit_store_capture_compound` and never walks the chain, so
`jit_capbase_uses` does not count it.

**SABOTAGE LEDGER**, each reintroduced, built and watched:

    r12 admitted to the claim        -> ML_CHECK in load_base, rc 134,
                                        on the first run
    load_base0 left as it was        -> corpus_diff --nolowmem 28/32,
                                        four programs truncated mid-run
    no epilogue re-derive            -> EVERYTHING GREEN (see below)
    no entry-stub re-derive          -> EVERYTHING GREEN, and vdjcmp
                                        says it emits ZERO bytes

**⛔ TWO UNFALSIFIABLE SITES, RECORDED RATHER THAN HIDDEN.** The last
two rows are the honest result, and they differ from each other:

 - the **entry-stub** re-derive emits NO BYTES corpus-wide, because no
   run that claims a capture base currently has an entry stub. It is
   not redundant - a stub is a fresh fragment entry, so the register
   holds the C caller's value there - it is UNREACHED. Kept: it costs
   literally nothing today and its absence would be a read through a
   garbage pointer the day a stub appears.
 - the **epilogue** re-derive DOES emit bytes (four programs). It is
   kept, and the argument that it is redundant is written at the site
   so a future measurement can act on it: the UN-pinned path already
   walks `ctx->captures` after calls and must get OUR captures, so "the
   call protocol restores it" is a premise this optimization INHERITS
   rather than adds, and a `CaptureSlots` is neither movable nor
   assignable and never grows after the snapshot. It costs the shapes
   that motivated the work nothing - the hot closure bodies make no
   helper call at all. Delete it only with a test that FAILS without it.

**LEVER**: `MYLANG_JIT_OFF=capbase`. **COUNTER**: `capbase`, bumped from
EMITTED code at each fragment entry that walked the chain - a
compile-time count would answer a different question. In the
`jit_counter_coverage` ratchet, so it cannot go inert unnoticed.
**TEST**: the `-rt` entry `jit: the capture base is walked ONCE per run,
not per access`, which asserts reach, RULE 2 (the lever off gives the
same answer and claims no base), and the invariant - a closure calling
ANOTHER closure between two accesses to its own capture, which really
does repoint `ctx->captures` and back.

## #113 - THE CAPTURE ROUND TRIP JOINS LEVER A (2026-08-27)

#112 removed the second walk of the ctx chain. What was left in
11_closure_counter's closure body was the value itself going through
memory twice for one `add`:

    +32  mov rax, [r13+0x18]   ; the capture's TYPE
    +39  mov rcx, [r13+0x0]    ; the capture's PAYLOAD
    +46  mov s0, rcx           ; store both into a temp...
    +53  mov s0.type, rax
    +60  mov rax, s0           ; ...that the next instruction reloads
    +67  add rax, 1
    +71  mov s1.type, <t_int>
    +82  mov s1, rax
    +89  mov rax, s1.type      ; reload the type just written
    +96  mov rcx, s1           ; reload the payload just written
    +103 mov [r13+0x0], rcx
    +110 mov [r13+0x18], rax

**LEVER A ALREADY REMOVES EXACTLY THIS SHAPE.** The capture ops were
simply not in its whitelists - the AUDIT-TABLE FOURTH SHAPE from
CLAUDE.md ("a table whose stale entry costs an OPTIMIZATION is not
self-announcing"), and this time the table was not even stale: the ops
had never been considered. Three pieces:

 - **LoadCaptureV is a PRODUCER.** Its payload is the whole value, so
   it loads straight into the bus register and, when the dst temp is
   dead after the consumer, neither slot store is emitted - which kills
   the capture's TYPE LOAD along with the type store it fed. Four
   instructions for a temp that lives for one. Its helper arm REJOINS,
   so it needs the slow-path bus reload the LoadElem* producers needed;
   the helper always writes the slot, including where the fast arm
   elided its store, so the slot is the right source there.
 - **StoreCaptureV is a CONSUMER** at `a`. ⛔ IT ALSO READS THAT SLOT'S
   TYPE WORD, which no other consumer does, so the arming site clears
   `skip_write` for it: a type-word read is a live read the liveness
   fixpoint cannot see, because it tracks SLOTS, not half-slots.
 - **THE BUS CARRIES A TAG.** `Fwd::res_tag` - the producer declares
   the type tag its own dst store wrote, the consumer writes that
   IMMEDIATE into the capture instead of copying the word back out of
   the slot. `jit_fwd_bus_tag` is an ALLOWLIST and the polarity is
   load-bearing: null means "read the slot", the status quo.

Both are per-INSTRUCTION refinements of per-OPCODE whitelists
(`jit_fwd_producer` refuses a boxed capture; `jit_fwd_consumer` refuses
a boxed or compound capture store), which keeps the opcode predicates
meaning "this opcode CAN forward" for the ratchet and the census - the
same split `jit_fwd_consumer` already made for its aliasing rules.

**THE BUG IT FOUND, AND THE CONFIGURATION THAT FOUND IT.**
`emit_ctx_chain` took an `AccScratch` - i.e. clobbered rax - for an
intermediate it did not need, every step's source being the previous
step's result. rax IS lever A's bus, so the walk emitted before a
capture store destroyed the forwarded value: **17682002402471496 where
20100 was expected.** The DEFAULT configuration cannot see it, because
#112's pinned base skips the walk entirely - only
`MYLANG_JIT_OFF=capbase` reaches it, and that case existed only because
#112's own test asserts RULE 2 with its lever off. Fixed in the walk
(it goes through its own output register now, same instruction count,
one fewer register touched, and the global arm still leaves the table
in `tbl`).

**MEASURED** (callgrind Ir, `OPT=1 ASSERTS=0`, scale3-minus-scale1):

    11_closure_counter   264,000,027 -> 252,000,027   -4.55%
    63_closures          536,000,000 -> 531,200,000   -0.90%
    78_typed_param_call  710,000,147 -> 702,000,147   -1.13%

Each delta is EXACTLY six instructions times the bench's own capture
call count (6 x 1,000,000 and 6 x 400,000). The closure fragment is
**53 -> 44 instructions per call across #112 and #113**, and its whole
capture round trip is now six:

    mov rax, [r13+0x0]        ; read the capture
    add rax, 1
    mov s1.type, <t_int>      ; s1 is RETURNED, so both stores stay
    mov s1, rax
    mov [r13+0x0], rax        ; payload out, from the bus
    mov [r13+0x18], <t_int>   ; tag out, as an immediate

**WALL CLOCK** (one interleaved `--baseline` A/B): 11_closure_counter
0.95x, 63_closures 0.97x, suite geomean **1.000x over 90**.

**⛔ AND THE SCATTER IN THAT RUN IS NOISE, PROVEN NOT ASSERTED.** 53 of
125 corpus programs' emitted code changed - `emit_ctx_chain`'s GLOBAL
arm changed too, and most programs have a global store - so "the ±5-8%
movers are noise" needed evidence, not a shrug. Per-iteration callgrind
Ir for the six worst (24_dict_lookup, 43_sieve, 03_int_arith,
37_range_builtin, 31_str_split_join, 09_fib_recursive) is **+0.0000%,
exactly identical**, as are 12_higher_order, 80_regs_int_08,
83_regs_int_40 and 14_array_subscript. The walk is the same three
instructions either way.

**SABOTAGE LEDGER.** One real, three unfalsifiable-and-recorded:

    emit_ctx_chain walks through rax   -> the REAL bug above; -rt fails
                                          via #112's capbase-off case
    bus tag claims t_int for a
      capture read                     -> every net green (see below)
    the consumer may trigger
      skip_write                       -> every net green (see below)
    a boxed capture admitted as a
      producer                         -> every net green (see below)

**⛔ THE THREE UNFALSIFIABLE ONES ARE UNREACHABLE BY CONSTRUCTION, NOT
UNDER-TESTED, and the difference took real work to establish.** The
only `cap_scalar` StoreCaptureV codegen emits comes from
`emit_typed_capture_update`, whose `a` operand is always the
IntBin/FloatBin temp it just produced - never a LoadCaptureV. So a
capture READ can never arm a capture STORE, which is what all three
rules guard. Each is kept with that argument written at its site: the
cost is nil, the failure prevented is a wrong VALUE, and the day
codegen emits a typed `capA = capB` they must already be right.

Getting there defeated TWO shape-eaters, both of which made an earlier
version of the tag test pass WITH the sabotage in - worth recording
because they are generic:
 - returning the copy's SOURCE reads the tag the capture READ produced,
   which is correct either way; the corrupted tag is the one the STORE
   wrote, so the CAPTURE has to be read back;
 - reading it back in the same call does not work either - the #97 step
   2a BYTECODE peephole rewrites `b = a; var t = b;` into
   `move t = <the store's source>`, so the capture is never re-read.
   Reading it at the TOP of the body, i.e. observing what the PREVIOUS
   call stored, is what defeats both.

**TEST**: the `-rt` entry `jit: the capture round trip forwards (lever
A) and its tag is fail-closed` - reach from `g_jit_fwd`, RULE 2 with
`fwd` off AND with `capbase` off (not redundant: the second is what
caught the rax clobber), and the bool tag case above.

## #114 - CLOSURE CREATION: 113 -> 85 Ir, and the profile that ranked it
## (2026-08-27)

**THE PROFILE FIRST, and it moved the target.** After #111-#113 the
closure CALL was down to 44 emitted instructions, so 63_closures was
re-profiled to find what actually dominated it. `jitprofile.py` said
**`elsewhere` was 144.7M of 285.5M - 50.7% of the program in C++
helpers**, against 8.6% for 11_closure_counter. That is not something
the emitted-code profiler can see into, so the next step was
callgrind's function view on a `DEBUG_INFO=1` build:

    main#0 fragment            95.0M  34.8%   475 Ir/iteration
    FuncObject::FuncObject     45.2M  16.5%   113 Ir/closure  <--
    jit_ret_norec              39.2M  14.4%    65 Ir/return
    move_assign (the DESTROY)  18.0M   6.6%    45 Ir/closure
    counter closure body       17.6M   6.4%    44 Ir/call
    the two factory bodies     15.4M   5.6%   ~38 Ir/call
    adder closure body         12.8M   4.7%    64 Ir/call   (see #115)
    jit_make_closure_ptr       10.8M   4.0%    27 Ir/closure

Creation + destruction is **32.7%** of the program, and its largest
piece is 113 instructions to build a closure with ONE int capture.

**⛔ THE PROFILER HAD TO BE FIXED BEFORE ANY OF THIS COULD BE READ.**
`jitprofile`'s per-fragment table was keyed by the fragment LABEL, and
every anonymous closure is `<lambda>` - so it reported ONE `lambda#0` of
30.4M for what is a 17.6M counter and a 12.8M adder, two different
closures with two different problems. The `--listing` path had its own
copy of the assumption in its filter and its printf. Fixed (commit
6e2beac) before the numbers above were trusted: same-labelled fragments
that RAN get a `~N` suffix; a dead twin (a chunk re-emitted after a
register conflict) is left alone so the common case still reads plainly.

**WHERE THE 113 WENT.** Line-level, per closure: 42 Ir of the ctor's own
code, **39 Ir of inlined `EvalValue` lifecycle**, 22 Ir of
`read_sym`/`CaptureSlots`/`get_root_ctx`, ~10 of pooled alloc. The 39
was the fat, and it was for copying ONE int:

    capture_slots.emplace_back(
        RValue(read_sym(ctx, cap.kind, cap.slot, cap.name)),
        ctx->const_ctx);

FOUR EvalValue lifecycle events, each with its own `type->t >= t_str`
test and potential type-erased PMF call - `read_sym` BOXES the slot's
address into an EvalValue, `RValue` unboxes it into a COPY,
`emplace_back` MOVES that copy into the LValue, and the temporary is
DESTROYED.

**THE FIX, in two parts, both "stop boxing a thing to immediately unbox
it":**

 - **`read_sym_lv`** - read_sym's LVALUE half. Every answer read_sym can
   give except an `UndefinedId` is `EvalValue(&some_slot)`, so a caller
   that wants the VALUE takes the slot and copy-constructs in place: one
   event instead of four. It returns null EXACTLY where read_sym answers
   `UndefinedId`, i.e. where the `RValue()` that followed would have
   THROWN, so the caller's fallback is the ERROR path and the two cannot
   disagree about a value. **`read_sym` is now written in terms of it** -
   one dispatch, not two copies free to drift, which is the whole reason
   the claim in its comment is safe to make.
 - **`EvalContext::root`** - inherited from the parent exactly like
   `frame` / `gfuncs` / `captures`, where `get_root_ctx` used to WALK
   the parent chain once per closure. The trade is one store per
   EvalContext against one walk per closure. **MEASURED BEFORE ASSUMING,
   because the trade only works one way:** under the VM+JIT the call
   protocol uses the native window and builds NO EvalContext per call -
   the ctor appears in 63_closures' profile at ~30k Ir total, against
   400,000 closure creations. Had contexts been per-call this would have
   been a net loss, and the profile is what said so.

**MEASURED** (callgrind Ir, `OPT=1 ASSERTS=0`):

    FuncObject::FuncObject   45,200,176 -> 34,000,148   113 -> 85 Ir
    63_closures whole        273,204,296 -> 262,005,166      -4.10%
    63_closures per-iteration                              -4.217%

**AND EVERY OTHER BENCH IS EXACTLY FLAT** - 11_closure_counter,
12_higher_order, 76_funcval_dispatch, 34_sort_custom_cmp, 35_map_filter
and 09_fib_recursive all read **+0.000%** per-iteration, to the
instruction. (11_closure_counter creates ONE closure for the whole
program, so a per-closure saving cannot show there - which is itself the
check that the change touches only what it claims to.)

**WALL CLOCK** (one interleaved `--baseline` A/B): 63_closures
**0.95x**, everything else 1.00x, suite geomean 1.000x over 90.

**WHAT IS LEFT IN THE 85**, measured, not guessed:
 - the `CaptureSlots` destructor's ~11 Ir - but the line attribution
   folds the inlined `~LValue`/`~EvalValue` into the `for`, so most of
   that is the work, not the loop;
 - `read_sym_lv`'s own 8 Ir (`ctx->frame->at(slot)`);
 - `reserve` + the default `CaptureSlots()` ctor together write
   ptr/n/cap twice, ~4 Ir of redundancy - not taken, because the class
   carries a load-bearing layout contract (the JIT reads its data
   pointer at offset 0) and 4 Ir does not justify touching it;
 - #109 (the inlined `intrusive_ptr::release`) still applies to the
   45-Ir DESTROY side, though here the destroy genuinely runs every
   iteration, so that one is an I-cache argument rather than an Ir one.

## #106 - a POWER-OF-TWO divisor skips the reciprocal entirely

`div_magic` declined only `d == 0/1/-1`, so `i % 4` paid the full
multiply-high sequence: a `movabs` of the magic constant, a
serialising 128-bit `imul`, and the truncation fixup - about ten
instructions. A power of two needs none of it.

**⛔ THE OBVIOUS FORM IS WRONG, AND SILENTLY SO.** `x & (2^k - 1)` is
the textbook mod-by-a-power-of-two and it does not implement MyLang's
`%`, which TRUNCATES: the sign follows the DIVIDEND (`-5 % 4` is `-1`,
not `3`), so a bare mask answers `3` for every negative dividend.
The correct any-sign forms bias first:

    tmp = x >> 63            all ones when x is negative
    tmp = tmp >>> (64 - k)   so tmp is (2^k - 1) or 0
    x  += tmp
    div:  x >>= k  (arithmetic)      -> trunc(x / 2^k)
    mod:  x &= (2^k - 1); x -= tmp   -> x - trunc(x/2^k)*2^k

Six instructions, no multiply. The DIVISOR's sign matters only to
`div` (negate the quotient) - truncating `%` is independent of it,
since `x % -4` and `x % 4` agree for every x.

**CAPPED AT k <= 31.** The mod arm masks with `and r64, imm32`, whose
immediate is SIGN-EXTENDED: a mask with bit 31 set would extend to
all-ones in the high half and mask nothing. Bigger powers - and
INT64_MIN, whose `|d|` is 2^63 - fall through to the reciprocal path,
which already handles them. The `k=31` / `k=32` pair in
`tests/functional/14_div_magic.my` straddles the cap, so the handled
edge and the first fall-through are both covered.

**MEASURED** (callgrind Ir, `OPT=1 ASSERTS=0`, scale-corrected so
compile time is excluded): **53_collatz -23.2%** (`n % 2` and `n / 2`
are its whole inner loop), **03_int_arith -7.4%**,
76_funcval_dispatch -1.5%, 57_bool_reduce -0.7%, 47_wordcount -0.4%.
No program regressed.

**REACH** is `MYLANG_JITSTATS`' `divpow2`, beside `divmagic`, so "the
tier ran" and "the reduction ran" cannot be confused: the 682-case
sweep reports 992 pow2 of 1364 reductions.

**WATCHED FAILING:** emitting the naive `and` without the bias makes
`14_div_magic.my` print `1420516668398` where the tree-walker prints
`106236609362`, and `corpus_diff` reports 32/33.

**THE REGISTER CENSUS CAUGHT TWO THINGS**, both worth knowing: the six
new `rax` uses needed justification tags (they are the div/mod
entry/exit PROTOCOL - the dividend arrives and the result leaves in
rax - not an ISA requirement, since this arm has no `imul`); and
naming the tag tokens in PROSE tripped the stale-tag check, because
the scanner reads them per LINE. The explanatory comment now describes
the tags without spelling them.

**PHASE 2 IS NOT BUILT.** A counted `for (var i = 0; i < n; i++)`
induction variable is provably >= 0, and for a non-negative dividend
the whole thing collapses to ONE instruction (`and` for mod, `shr` for
div). That needs a "non-negative" bit stamped by the for-range
specializer - a range fact, not a peephole - and is a separate change.
