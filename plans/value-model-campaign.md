# #60 — the value-model perf campaign (reduce boxing / refcount / dispatch)

The gap to native C++ is `my/cpp ~4.6x` and it is NOT dispatch (the VM removed
that) - it is the value model's per-op cost. This file records the profiling
that opens the campaign and the candidate targets, so the maintainer can
prioritize (his call) and each step stays small + measured.

## Profiling (callgrind, build-perf = OPT=1 ASSERTS=0, 2026-07-20)

### 23_dict_insert (dict-bound, NOT the core value model)
Total 213.6M Ir. Top:
- `std::_Hashtable::{_M_insert_unique_node, _M_emplace, find, _M_rehash}` ~40%
  - the chained unordered_map's node alloc + probe.
- `TypeDict::subscript` ~13% (across TUs) + `vm_subscript_store` ~7% + `slot_rmw`
  ~5% - the store path.
- `make_const_clone` (the KEY FREEZE on every insert) ~12%.
- `pool_alloc_one` + `__memset` ~9% - node allocation.
=> Dominated by the CHAINED HASHTABLE + the key freeze. This is the
   long-standing **flat open-addressing dict** item (H2 v2, maintainer
   sign-off) - a container-specific fix, not the general value model. The
   node-pointer-STABILITY requirement (held LValue*/iterators) is why the flat
   map was rejected before; revisit with that constraint.

### 35_map_filter (general-value + callback, the CORE value model)
Total 592.4M Ir. Top:
- CALLBACK DISPATCH ~30%: `vm_run_chunk` re-entry (~20% + inlined) +
  `VmInvoker::invoke` (~18% across TUs) + `vm_run_chunk::EntryGuard::~EntryGuard`
  (~4.6%). Each map/filter ELEMENT re-enters vm_run_chunk (a fresh activation
  lookup + entry guard). The window is pushed once per loop (VmInvoker), but the
  LOOP RE-ENTRY is per element.
- VALUE COPY/MOVE ~11%: `EvalValue::operator=(&&)` ~7% + `operator=(const&)`
  ~3.7% - the tagged-union copy (branch trivial vs the type-erased ref ops).
- DISPATCH ~4%: `num_bin_op` (the PMF call through the Type virtual - the `x*2`
  in the callback; M8 avoids it only for a proven-int/float NODE, not a dyn/
  general callback param).
- `arr_elem_at` ~4.4%, result `vector::_M_realloc_insert` ~2.5%, `LValue::put`
  ~2%, EvalValue default-ctor (`ValueU()`/`type(t_none)`) ~1.8%.

## The generalizable levers (candidate targets - maintainer prioritizes)

1. **Callback re-entry (VmInvoker / higher-order builtins).** ~30% of
   map/filter (and sort/find/make_dict). Each element re-enters vm_run_chunk
   (activation lookup + EntryGuard). Idea: a VmInvoker that runs the callback
   body WITHOUT the full vm_run_chunk boundary re-entry per element (the window
   is already pinned once) - drive the callee chunk in a leaner loop. Narrow
   (higher-order builtins) but a big % there. LOW-risk, well-scoped.

2. **`num_bin_op` / Type-virtual dispatch for DYN/general operands. DONE
   2026-07-20 (`vm_num_binop`, vm.cpp).** The ~7 boxed BinOpV/CompoundV/CmpV +
   compound-store + dyn-inc-dec sites now take an int-int FAST PATH: both
   operands plain int -> a switch on the Op ENUM (comparison yields int 0/1,
   CmpV wraps is_true); else the exact num_bin_op PMF fallback (byte-identical,
   div0/shift/type throws intact). ML_NOINLINE (one copy, no vm_dispatch growth,
   a direct call replacing the indirect PMF). The map/filter/sort dyn hot spot
   was actually the boxed COMPARISON-as-a-VALUE (`x%3==0` return), not general
   dyn arith. Measured (callgrind Ir): 34_sort -7.4%, 67_make_dict -5.6%,
   35_map_filter -5.0%; pure loops neutral. -rt 1562/1562 + differential
   1399/1399 (2 new boxed-dyn tests), nested_fuzz 400 all-agree. The tree-walker
   keeps the plain PMF path (the differential oracle). A native typed-VALUE
   compare op (bool result, no box) would remove `x%3==0`'s box ENTIRELY - a
   possible M8 follow-up, separate from this lever.

3. **EvalValue copy/move cost.** ~11% here, EVERYWHERE. The tagged-union copy
   branches trivial vs the type-erased ref ops. Hard to beat without changing
   the value model; small wins possible (e.g. the default-ctor zeroing, move
   paths). HIGH-effort/low-margin - probably last.

4. **The flat open-addressing dict (H2 v2).** ~40% of the dict benches.
   Container-specific (not the "value model" per se), needs the maintainer's
   sign-off (node-stability). Biggest single win for dict-heavy code.

5. **intrusive_ptr retain/release churn.** Shows as `TypeImpl<T>::{copy_ctor,
   dtor}` (~3% in dict). Reduce copies of reference EvalValues on hot paths
   (borrow-by-ref where a copy is currently made). Case-by-case.
   **IN PROGRESS 2026-07-20:** `TypeArr::subscript` + `TypeArr::slice` bound
   `const SharedArrayObj &arr = what.get<SharedArrayObj>()` - `what` is const, so
   `get<>()`'s by-VALUE const overload COPIED the array handle (retain+release)
   on EVERY general-array index/slice. Changed to `get_ref<>()` (borrow); the one
   mutable path (an LValue-array element's back-pointer) re-derives the mutable
   vec through the base LValue* (a ref, no copy). Measured (callgrind Ir):
   62_dict_word_count -2.31%, 47_wordcount -1.85%, 15_array_slice_readonly
   -1.21%, 16_array_slice_write -1.13%; flat arrays + string slices neutral
   (flat reads use native LoadElem*, string slice is TypeStr::slice). Then the
   STRING ops: the read-only TypeStr methods (is_true/len/to_string/hash/
   use_count/is_slice/intptr/clone + subscript/slice) took a CONST `a` and the
   comparisons (eq/noteq/lt/gt/le/ge/add) a CONST `b`, so `get<SharedStr>()`
   COPIED the handle per call - changed to `get_ref<>()` (borrow; `add`'s
   mutable `a` and the non-const-`a` comparison LHS already return a ref, left
   as-is). Additional: 62_dict_word_count -2.37% (cumulative ~-4.6% with the
   array borrow), 29_str_slice_readonly -2.49% (TypeStr::slice, was neutral),
   47_wordcount -0.90%. Then the DICT ops (TypeDict: subscript + the read-only
   len/eq-b/noteq-b/hash/to_string/pretty/is_true/use_count/intptr/clone): same
   const-handle copy -> get_ref borrow. A dict doesn't COW, and operator->/get()
   on a const intrusive_ptr yield a MUTABLE pointee, so subscript's borrow is
   sound for read AND write (the auto-vivify/freeze-insert go through the
   pointee). eq/noteq's non-const `a` LHS already returns a ref (left as-is).
   Smaller wins (23_dict_insert -0.42%, 62_dict_word_count -0.32%,
   26_dict_iterate -0.20%; make_dict/dict_member neutral - they use the typed
   DictLoad path / make_dict builtin, not TypeDict::subscript). -rt 1566/1566 +
   differential 1403/1403, nested_fuzz. DONE for all 3 container types. STILL
   TODO: vm_store_base's dict-store base copy (~40M in the dict bench, murky -
   callgrind inlined attribution).

## ===== STATUS: callback re-entry DONE 2026-07-20 (a + b landed) =====
Both steps landed and measured (callgrind whole-program Ir, scale 1, vs the
pre-#60 baseline):
- **(a) reentrant vm_run_chunk fast path** (commit f97186c): skip
  vm_enter_invocation_fast + the CtxGuard store per element; VmInvoker/
  vm_try_invoke own g_current_ctx for the loop; arity fields hoisted to the
  VmInvoker ctor. -> 35_map_filter -2.03%, 34_sort_custom_cmp -1.60%,
  67_make_dict -1.08%. The per-element EntryGuard DTOR (4.6%) was UNTOUCHED (a
  runtime gate can't drop a local's dtor call) - exactly the (b) trigger.
- **(b) vm_dispatch extraction** (this commit): split the dispatch loop into a
  file-local vm_dispatch(chunk, ctx, act); VmInvoker::invoke / vm_try_invoke
  call it DIRECTLY (no EntryGuard, no enter_fast, no CtxGuard). -> an ADDITIONAL
  -8.36% / -7.53% / -5.50%, for TOTAL vs baseline **-10.21% / -9.01% / -6.52%**.
  The EntryGuard dtor is gone from the callback path. PURE LOOPS NEUTRAL (the
  key risk - a hot-function split perturbing dispatch layout): I-count flat
  (+-0.17%), wall-clock min ratios 0.977-1.006 over 21 interleaved reps
  (01_while/02_for/03_int_arith/43_sieve/09_fib). Verified: -rt 1560/1560 + VM
  differential 1397/1397; nested_fuzz 400 all-agree; a sort-comparator that
  divides by zero shows the byte-identical bad()<-main() backtrace (caught +
  uncaught).
NEXT lever: #2 num_bin_op dyn fast-path (broad), then #4 flat dict (needs
sign-off), #3 EvalValue copy (hardest). See the target menu above.

## ===== FOLLOW-UP: native typed-VALUE compare DONE 2026-07-20 =====
The lever-2 profiling found the dyn hot spot was the boxed COMPARISON-as-a-VALUE
(`x % 3 == 0` returned) - M8's native typed compare only existed as the BRANCH
form. New `CmpIntV`/`CmpFloatV` ops (bytecode.h/vm.cpp/codegen.cpp) compute a
2-operand typed int/float compare into a real BOOL slot, no box; the boxed CmpV
stays for dyn/string operands + >2-operand chains. `try_native_cmp_value`
(compile_boxed_expr) reuses compile_int_cond/float_cond. Bodies are ML_NOINLINE
(the loop-body text rule - inlining cost 55_float_sum +0.6% I-count / ~3% wall
with no bytecode change; off-frame helpers restored I-count neutrality).
Measured (callgrind Ir vs lever-2): 34_sort -10.5%, 35_map_filter -11.5%
(the boxed CmpV/num_bin_op/is_true GONE); cumulative from pre-#60: sort -20%,
map_filter -26%. -rt 1565/1565 + differential 1402/1402 (3 new tests: typed
int/float compare-value, boxed chain); nested_fuzz 400 all-agree. OPEN: a
residual 55_float_sum WALL signal (~a few %, I-count-neutral) is the
vm_dispatch-growth layout tax every new op pays (WSL2 has no PMU to diagnose;
maintainer's call whether the broad compare-value win justifies it).

## ===== EXECUTION GUIDE: callback re-entry (self-contained, compact-safe) =====
Maintainer's pick 2026-07-20: do (a), MEASURE, then (b), MEASURE. HEAD at write
time = 3780fcf. Build/test: `make -j TESTS=1 OPT=0 BUILD_DIR=build-dbg` (debug
+ASan) -> `./build-dbg/mylang -rt` (expect 1560/1560 + `VM differential ...
1397/1397`, exit 0); perf binary `make -j OPT=1 ASSERTS=0 BUILD_DIR=build-perf`;
`python3 tests/nested_fuzz.py --mylang build-dbg/mylang --count 500`.

### THE COST (callgrind, 35_map_filter, build-perf)
`VmInvoker::invoke` re-enters `vm_run_chunk` PER ELEMENT. The window is pushed
ONCE (VmInvoker ctor), but each re-entry re-pays the per-invocation SETUP:
- `vm_run_chunk::EntryGuard::~EntryGuard` 4.56% (non-inlined LTO clone; a NO-OP
  for a re-entry - pushed=false, swapped=false - but the dtor CALL costs).
- the STEP-1 `CtxGuard` (save/restore `g_current_ctx`) - REDUNDANT: it is the
  same invoke_ctx (`c_`) every element.
- `vm_enter_invocation_fast` (a call; fast path is cheap but per-element).
- `pending_key` `unique_ptr` ctor/dtor 1.86% (always null for a callback with
  no CachedCallV).
HONEST SIZING: the reducible OVERHEAD is ~10% of map/filter (EntryGuard 4.6% +
pending_key 1.9% + CtxGuard + enter_fast, unmeasured ~3-5%). NOT ~30% - that
figure lumped in the callback BODY work + the param BIND (both inherent). So
target ~10% on map/filter/sort/find; measure honestly, don't overclaim.

### CODE MAP (vm.cpp, line numbers approx - re-grep)
- `VmInvoker` ctor ~1839: gates (g_exec_engine==Vm && g_vm_act &&
  invoke_ctx && !const_eval && obj.func->vm_chunk); `act_=g_vm_act`;
  `c_=act_->invoke_ctx.get()`; `cck_=desc_->vm_chunk`; `w_=act_->push_window(
  total, cck_, /*boundary=*/true)`; `saved_caps_=c_->captures; c_->captures=
  &obj.capture_slots; ready_=true`.
- `VmInvoker::~VmInvoker` ~1843: `c_->captures=saved_caps_; act_->pop_window()`.
- `VmInvoker::invoke(argv,n)` ~1851: ARITY check (n vs [min_args,nparams]);
  bind (fast_bind copy loop OR coerce loop) into `w_->at(i)`; `c_->flow->type=
  none`; `try { vm_run_chunk(*cck_, *c_); } catch(Exception&e){
  vm_capture_desc_frame(e,desc_); throw; }`; if `g_vm_exc_pending` capture+
  rethrow; read `c_->flow->value` iff type==ret; ref-release scan over
  `cck_->ref_slots`.
- `vm_try_invoke` ~1738: the SINGLE-SHOT twin (eval_func's gate) - same shape,
  a `Restore` dtor pops the window. Apply the SAME optimization here.
- `vm_run_chunk` ENTRY ~2286: `chunk=&chunk0`; `EntryGuard entry_guard`
  (swapped/pushed); `act=*vm_enter_invocation_fast(chunk, local_act,
  swapped, pushed)`; `CtxGuard ctx_guard(&ctx)` (sets g_current_ctx=&ctx,
  restores); `code=chunk->code.data(); pc=0`; `unique_ptr<PureCacheKey>
  pending_key`; lambdas cur_rec/diter/dyiter; then the P8 exception boundary
  + the dispatch loop.
- `vm_enter_invocation_fast` ~1934: `if (a && !a->no_recs() &&
  a->back_rec().run_chunk==chunk) return a;` else the NOINLINE slow path.

### STEP (a) - the `reentrant` fast path (contained, LOW-risk). Measure first.
1. `vm_run_chunk(const Chunk&, EvalContext&, bool reentrant=false)` (decl+def).
   The decl is in a header? -> it's `static`/file-local in vm.cpp (grep - it is
   only called within vm.cpp: vm_run, vm_try_invoke, VmInvoker::invoke). Add the
   param with a default so existing calls are unchanged.
2. When `reentrant`: `VmActivation &act = *g_vm_act;` and DO NOT construct the
   EntryGuard nor call vm_enter_invocation_fast (VmInvoker owns the window +
   activation and never swapped g_vm_act). Also DO NOT construct the CtxGuard
   (g_current_ctx is already `&ctx`, set once by VmInvoker - see step 3).
   Structure: the guards are LOCALS, so to truly skip them, split -
   `if (reentrant) { act=&*g_vm_act; } else { <EntryGuard+enter_fast+CtxGuard> }`
   won't compile (guard scope). CLEANEST: pull the guarded setup into the
   NON-reentrant branch by making EntryGuard/CtxGuard members of a small struct
   constructed only in the non-reentrant path, OR (simpler) keep the guards but
   gate their WORK on `!reentrant` - this removes the enter_fast call + the
   g_current_ctx store, but NOT the dtor calls. If the measured win is mostly
   the dtor calls (EntryGuard 4.6%), (a-gate) is insufficient -> go to (b).
   => So (a) as a runtime GATE is the CHEAP experiment; if it wins enough, done;
   if the EntryGuard dtor dominates, (b) is required. MEASURE to decide.
3. VmInvoker owns g_current_ctx for the loop: ctor `saved_gctx_=g_current_ctx;
   g_current_ctx=c_;` dtor `g_current_ctx=saved_gctx_;` (add a member). Then
   invoke() calls `vm_run_chunk(*cck_, *c_, /*reentrant=*/true)`. Do the same in
   vm_try_invoke (set g_current_ctx around its vm_run_chunk, reentrant=true).
   NB a nested callback (a callback that itself calls a builtin with a callback)
   still works: the inner VmInvoker saves/restores g_current_ctx around its own
   loop.
4. Tiny free win: hoist VmInvoker::invoke's ARITY check to the ctor (n is loop-
   fixed - store nparams/min_args, check once). Skip the per-element check.
5. MEASURE (1-vs-1, per the discipline): build-perf callgrind 35_map_filter +
   34_sort_custom_cmp + 39_find_builtin before/after; also `bench/run.py
   --filter map,filter,sort,find` (needs the caches fresh - the new --recompute
   step). VERIFY: -rt 1560/1560 + differential; nested_fuzz 500 (VmInvoker is on
   the sort/map/filter path); an exception THROWN in a callback still shows the
   right frame (a `catch` around a sort comparator that throws - check the
   backtrace). COMMIT if it wins + is green.

### STEP (b) - extract the dispatch loop (the full fix). Do iff (a) is small.
Split `vm_run_chunk` so the per-element re-entry pays ZERO setup:
- `vm_dispatch(const Chunk &chunk0, EvalContext &ctx, VmActivation &act)` = the
  ENTIRE current dispatch loop + its locals (code, pc, pending_key, the lambdas,
  the P8 exception boundary/landing pad). Returns when the invocation's boundary
  ReturnV/Halt is hit (as today).
- `vm_run_chunk(chunk, ctx, reentrant=false)` = (non-reentrant) EntryGuard +
  vm_enter_invocation_fast + CtxGuard, THEN `vm_dispatch(chunk, ctx, act)`.
- VmInvoker::invoke = per element: bind + `c_->flow->type=none` +
  `vm_dispatch(*cck_, *c_, *act_)` DIRECTLY (no guards - VmInvoker owns the
  window + activation + g_current_ctx). vm_try_invoke likewise.
HAZARDS: (1) clang's indirect-goto rule - the dispatch loop's labels live inside
one scope with a live non-trivial local (pending_key); keep pending_key INSIDE
vm_dispatch. (2) The `code`/`chunk`/`pc` loop state resets per call - fine, it is
local to vm_dispatch. (3) The P8 exception boundary (the try/catch landing pad
in vm_run_chunk) must move INTO vm_dispatch (a callback can throw/catch). (4)
The `cur_rec/diter/dyiter` lambdas capture `act` by ref - pass act to
vm_dispatch. (5) MEASURE: this is a HOT-path refactor - a `-vd` byte-identical
check does NOT apply (no codegen change), so lean on -rt + differential +
nested_fuzz + the exception/backtrace tests + a 1-vs-1 bench A/B. This is a big
diff to a shared hot function - do it carefully, ONE commit, fully tested.

### AFTER: next levers (see the target menu above) - #2 num_bin_op dyn fast-path
(broad), then #4 flat dict (needs sign-off), #3 EvalValue copy (hardest).
