# VM fallback elimination + optimization — road to 5× CPython

**Goal:** lift the `-vm` geomean from **~4.0× CPython** to **5×**.

**Method:** ran `mylang -vd` on all 62 `bench/my/*.my` and recorded every
AST-fallback op (`eval.stmt` = `EvalStmt`, `eval.slot` = `EvalToSlot` — the two
ops that re-enter `node->eval`). Cross-referenced with the per-benchmark
VM/CPython ratio (`bench/run.py --vm`, release + `ASSERTS=0`). All numbers below
are that run (2026-07-05, HEAD `c05d6aa`).

## The geomean math (why one outlier is NOT the answer)

The geomean is a *product* over ~60 benchmarks, so a single benchmark moves it
by `ratio^(1/60)`. Concretely, measured:

- All 60 timed: geomean **0.266 (3.76×)** — `run.py`'s 61-set prints 4.0×; the
  difference is one near-zero const-fold bench it drops.
- **42_exceptions is 24.5× (VM *slower* than CPython)** — the worst by far — yet
  removing it only lifts the set to **4.06×**. Fixing it to 1.0× → **3.97×**;
  to 0.3× → **4.05×**. So exceptions is worth **~5%**, not the story.
- To reach **5× (geomean 0.20)** from 0.246 (the 59 non-exception benches) the
  product of all improvements must be `0.20/0.246 = 0.81` overall, i.e. roughly
  **double ~15 laggard benchmarks** (or 20 by 1.7×). This is a BROAD-category
  job, not one hero fix.

So the roadmap is organized by CATEGORY (how many benchmarks it moves), with
exceptions as a standalone (it's the vm-endgame "kill C++ exceptions" goal and a
real 24×, just not a geomean mover).

## Part A — the fallback inventory (what `-vd` found)

40 of 62 scripts are already 100% native. The 22 with fallbacks, grouped by the
underlying construct (● = hot / in the amplifier loop, ○ = cold one-time setup):

- **D1 · dict store** `d[k]=v`, `d.k=v` — benches 23, 24, 26, 27, 47, 62 (six).
  `StoreElem*` handle FLAT arrays only; a dict subscript store has no op → the
  whole build loop is `eval.stmt`. ●
- **D2 · dict read typed** `d.k`/`d[k]` on `dict<_,int/float>` — bench 25.
  `member.v`/`subscript.v` BOX even when inference proved the value is int;
  the tree-walker has `MemberExpr::eval_int` (`dict_present_value`) but the VM
  has no typed dict read. ●
- **D3 · dict foreach** `foreach(k,v in d)` — benches 26, 47, 62. Native foreach
  lowers only a flat `array<int/float>` single var; a dict foreach is
  `eval.stmt`. ●
- **B · bool array r/w** `a[i]=true`, `if(a[i])` — benches 43, 56, 57.
  `StoreElemInt/Float` + `LoadElemInt/Float` cover int/float; a flat
  `array<bool>` element has NO op, so the whole sieve is tree-walked. ●
- **F · foreach unpack/indexed** `foreach(x,y in a)`, `foreach(i,e in a)` —
  benches 19, 20. Native foreach = single non-indexed var; unpack/`indexed`
  fall back. ●
- **C · closure / indirect call** `c()` (a `FuncObject` in a slot) — bench 11.
  `CallV` fires only for a call devirtualized to a GLOBAL user-func slot; a call
  through a local/captured func value → whole loop `eval.stmt`. ●
- **X · exceptions** `try/catch` + `throw` — bench 42 (24.5×). try/catch is
  `eval.stmt`; every `throw` is a C++ `throw` (~1.6µs: heap + DWARF unwind).
  100k throws = the 24×. ●
- **S · string element** `s[i]`, build — benches 29, 30, 31, 32. `LoadElem*` are
  array-only; a string subscript is boxed `subscript.v`; `parts[i]=str(i)` (an
  array<str> store) is `eval.stmt`. ●
- **G · general/str array store** `a[i]=<non-scalar>` — benches 46, 20, 31, 32,
  47. `StoreElem*` store a scalar into a FLAT vector; storing an array/struct
  VALUE into a general array element → `eval.stmt`. ●
- **M · multi-assign / IdList** `a,b = ..`, `a,b,c = [..]` — benches 22, 06. An
  `IdList` assignment target has no native lowering. ● (22) / ○ (06)
- **R · call-in-global-assign** `r = f(n) + 900` — bench 10. The loop assign
  compiler doesn't handle `global = <call-containing expr>`, so a recursion-
  driving loop is `eval.stmt` (the recursion is tree-walked). ●
- **A · append value self-eval** `push(a,i)` — bench 13. `CallBuiltinLV` fires
  (native dispatch) but `push` self-evals its VALUE arg (a `node->eval` of `i`).
  ●
- **cold-only (ignore)** — most `a=[..]`, `d={}`, `func f(){}`, `sort`/`map`/
  `filter` decls and one-shots: a one-time `eval.stmt`; the heavy builtin
  dominates its own dispatch. ○

## Part B — prioritized fix roadmap

Ordered by (impact × breadth) ÷ risk. Each step: one op or lowering, gated on
the 1156-test differential + RECYCLE+ASan, benched `--vm` before/after.

### P1. Bool array element read/write — `StoreElemBool` / `LoadElemBool`
*Benches: 43_sieve (1.08→~0.4), 56_sieve_bool, 57_bool_reduce. LOW risk.*
Mirror the existing `StoreElemInt`/`LoadElemInt` exactly, over `bvec` (the
`unsigned char` flat store) with the same bounds/negative-wrap/COW/hash-
invalidate. `a[i]=true/false` and `if(a[i])` in the sieve become native (the
inner `while(j<n){primes[j]=false;j+=i;}` is ALREADY native but for that one
store). Highest value-to-effort ratio; **do first**.

### P2. Dict subscript/member store — `DictStore`
*Benches: 23, 24, 26, 27, 47, 62 (six). MED risk.* The biggest category. A
`d[k]=v` / `d.k=v` op: read the dict slot, evaluate key+value operands (boxed),
call the shared `TypeDict::subscript(..,for_write=true)` path (auto-vivify,
key-freeze, COW) the tree-walker uses — a runtime FUNCTION, no `node->eval`.
Compound `d[k]+=v` too. This un-flattens the dict-build loops gating 6 benches.

### P3. Typed dict READ — `DictLoadInt` / `MemberInt` (+ float)
*Benches: 25 (.77), and every dict-of-scalars read loop. MED risk.* When
inference proved `dict<_,int/float>`, read the present-key value directly
(`dict_present_value` → the scalar) into a typed slot instead of `member.v`/
`subscript.v` boxing. The VM analogue of the tree-walker's `eval_int`
fast path (which the VM lacks). Turns `s += d.alpha + d.beta + ..` from boxed
member+add into typed reads + `IntBin`.

### P4. General array element store — `StoreElemValue`
*Benches: 46_matrix (.42), 20, 31, 32, 47. MED risk.* `a[i]=<value>` where the
element is a non-scalar (array/str/struct) or the array is general: evaluate the
value boxed, use the tree-walker's element-lvalue + COW store. Pairs with P2/P3
to make the matrix/row-build and `parts[i]=str(i)` loops native.

### P5. foreach — unpack, indexed, and dict
*Benches: 19, 20 (.69), 26, 47, 62. MED/HARD.* Extend the native-foreach
lowering: (a) tuple unpack `foreach(x,y in a)` over a flat/general array;
(b) `indexed`/`foreach(i,e in a)`; (c) dict `foreach(k,v in d)` (snapshot the
bucket walk). (c) is the hard one — a dict has no flat index; lower to an
iterator cursor op. Big for the dict-heavy benches (overlaps D3).

### P6. Indirect / closure calls — `CallValue`
*Bench: 11 (.60). MED.* A call whose callee is a `FuncObject` VALUE in a slot
(local/captured/global-var, not a devirtualized global-func slot): read the
slot, check `is<FuncObject>`, evaluate args into a register run, `vm_call_func`.
Generalizes `CallV`, so `s += c()` loops go native. (The closure `start++`
on a captured var is a separate small fallback — a capture-slot store.)

### P7. String element read — `LoadStrChar` / typed
*Benches: 29, 30 (.44), 31. MED.* `s[i]` → the char (or, for `ord(s[i])`
loops, fuse to an int char code). String build (`parts[i]=str(i)` then `join`)
overlaps P4.

### P8. VM-level exceptions — no C++ throw for user `throw`
*Bench: 42 (24.5×). HARD, high-symbolic-value (the vm-endgame goal).* Lower
`try/catch/throw/rethrow` to VM control flow: a `try` pushes a handler record
(catch-label + catch-var slot + finally-label) onto a VM handler stack; `throw`
of a struct sets a "pending exception" and jumps to the top handler's catch
dispatch (match by struct name) WITHOUT a C++ `throw`; `finally` runs on the
unwind path. Only RUNTIME-error exceptions (div-by-zero) still use C++ throw
(rare). Removes ~100k C++ throws in the bench → 24.5× should collapse toward
~1×. It's ONE bench for the geomean (~5%) but the single most-dramatic per-bench
win and the stated endgame direction — schedule after the broad wins.

### P9. Multi-assign / `IdList` — native
*Benches: 22 (.64), 06.* Lower `a,b = expr` / `a,b = [..]` to the element-spread
the tree-walker's `handle_single_expr14` does, over slots.

### P10. `push`/`append` plain-value ABI + call-in-assign
*Benches: 13 (.79), 10 (.64).* (a) give `push`/`append` a value-ABI plain-append
path so the value arg isn't self-eval'd (2b's emplace already killed the ctor
case). (b) Extend the loop-body assign compiler to accept `global = <expr with a
CallV>` so 10_recursion_deep's driver loop goes native (the recursion stays a
real call, but stops being tree-walked wholesale).

## Part C — VM optimizations (native but sub-optimal)

Independent of fallbacks — these speed code that is ALREADY native:

1. **Typed operands into member/subscript/builtin reads.** P3 is the biggest
   instance: many `member.v`/`subscript.v`/`call.blt.v` feed a typed arith chain
   but box. A typed-read variant (int/float dst) removes a box+unbox per
   hot read. 40_math_builtins (.42, 0 fallbacks) is gated by boxed builtin
   results feeding typed adds.
2. **Dispatch via computed-goto** (`&&label` threading) instead of the `switch`
   in `vm_run_chunk`, on GCC/clang. Removes the range-check + the single
   indirect-branch misprediction hub; classic 10–20% on a dispatch-bound loop.
   Keep the `switch` fallback for MSVC.
3. **`CallV`/`CallBuiltinV` arg movement.** Args are copied slot→buffer→callee.
   Where the callee reads a contiguous frame run, pass the run by pointer (the
   `VmArgs` view already does this for user calls; extend to the builtin ABI so
   `call.blt.v` doesn't `EvalValue`-copy each arg).
4. **Fuse `x = x % C`** (a constant modulo in nearly every bench's `s = s %
   1000000007`). Already native (`IntBin`), but a `ModConst` immediate-second-
   operand form saves the operand decode. Minor.
5. **Avoid re-boxing a bool comparison result** that immediately feeds a branch
   (largely done via `JumpUnlessIntCmp`; audit `bin.v`+`JumpIfFalse` pairs for a
   fused `cmp.v`+branch).

## Suggested execution order

P1 (bool arrays) → P2 (dict store) → P3 (typed dict read) → C1/C2 (typed reads +
computed goto, broad) → P4 (general store) → P5 (foreach) → P6 (closures) → P7
(strings) → P9/P10 (multi-assign, append) → P8 (VM exceptions, last — hardest,
one bench). Re-bench `--vm` after each; expect the dict + bool + typed-read
cluster (P1–P4, C1) to do most of the 4.0→5× lift since it moves ~12 benchmarks.

Each step is small, differential-gated, RECYCLE+ASan-clean, and doc-synced in
the same commit — same cadence as Phase 2a/b/c.
