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

2. **`num_bin_op` / Type-virtual dispatch for DYN/general operands.** ~4% here,
   broad across dyn-heavy code. M8 removed it for proven scalars; a dyn operand
   still pays the PMF. Idea: a fast-path in num_bin_op for the common int/float
   dynamic pair before the PMF, or cache the resolved op. Broad, MEDIUM-risk
   (correctness of the promotion rules).

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

## RECOMMENDATION (smallest high-value first step)
**#1 (callback re-entry)** - it is the single biggest, most self-contained cost
in a real bench (map/filter/sort), LOW-risk, and does not touch the value
model's core invariants. Then #2 (dyn dispatch fast-path) for breadth. #4 (flat
dict) is the biggest raw win but needs a design + sign-off; #3 (EvalValue copy)
is the hardest and last. Awaiting the maintainer's pick before implementing (he
prioritizes on value; each step 1-vs-1 measured per the bench discipline).
