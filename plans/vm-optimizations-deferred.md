# VM optimizations — deferred (parking lot)

These are perf ideas **parked** while we pursue the `.myv` serialization endgame
(zero AST fallback → exceptions first; see `plans/vm-exceptions.md` and the END
GOAL in `plans/bytecode-vm.md`). They are perf-only, **orthogonal to the
bytecode format** (none blocks or is blocked by `.myv`), so we return to them
after the fallbacks are gone. Ordered by the assessment below, not by ID.

Ground rules for ALL of these (from the maintainer): a change must be
**perf-neutral-or-better in the worst case**, verified by the `bench/run.py --vm
--baseline <same release binary>` wall-clock geomean (reliable on the dev WSL2 —
benches are consistent — even though it has **no PMU**; see
`[[vm-dispatch-frontend-regression]]`). Correctness is always gated by the 1220
`-rt` VM differential + the `nested_fuzz.py` three-way fuzzer.

---

## C2 — computed-goto (threaded) dispatch  ·  the one real lever, RISKY here

Replace the central `switch (in.op)` in `vm_run_chunk` with GCC/clang
labels-as-values: a `static const void* table[]` of per-op code labels, and each
handler ENDS with its own `goto *table[chunk.code[pc].op]`. Instead of ONE
shared indirect dispatch branch (which sees every op→op transition blended and
mispredicts), there is one dispatch branch PER handler, each of which the CPU's
indirect predictor can specialize to the local bytecode correlations (after
`IntBin` usually `IntBin`/`ForLoopStep`, …). Classic 10–20% on dispatch-bound
loops (Ertl & Gregg 2003); also drops the switch's implicit bounds check.

- **GCC/clang only** → ships behind `#if defined(__GNUC__)`, `switch` kept for
  MSVC. Two dispatch paths to keep in sync.
- **Pair with a cold-handler split**: hoist rarely-taken code (error throws,
  big cold ops) out of the hot loop body so the hot handlers pack densely into
  I-cache.
- **Uncertain HERE**: modern ITTAGE predictors already predict the single switch
  branch well (CPython 3.11's win was small); and the front-end effect is
  **unmeasurable on WSL2 (no PMU)** — only wall-clock is available. So it's a
  MEASURED EXPERIMENT (implement, verify the 1220 differential, keep only if the
  `--vm` geomean is neutral-or-better), NOT a blind commit. ~64-handler
  mechanical conversion (each `case`→label, each `break`→threaded dispatch,
  `in`/ `pc` re-read per label) → bug-risk, but differential-caught.
- **This is also the DIRECT fix** for the 2026-07-08 dispatch-slowdown
  regression (a bigger op set stops regressing the hot ops' branch prediction
  once each op has its own dispatch branch). So it should be done BEFORE adding
  more ops (e.g. C4).

## C3 — builtin arg-view ABI  ·  MARGINAL, broad churn

`CallBuiltinV` copies the n pre-evaluated args from the frame run into a stack
`EvalValue[8]` before calling `func_v(ctx, ExprList*, args, n)`. Idea: pass the
frame run by pointer/view (the `VmArgs` view already does this for user `CallV`)
so no per-arg `EvalValue` copy.
- **Low value-to-churn**: the copy is cheap for the SCALAR builtins that
  dominate hot loops (only non-trivial string/array args pay a refcount bump),
  and the change touches all **84** `func_v` signatures.
- Its genuinely-valuable half — freeing the builtin ops' `Instr::node` (the args
  `ExprList` carried only for per-arg error carets) — is the **builtin
  loc-handle refactor**, which belongs with the AST-free / `.myv` work
  (`plans/vm-fallback-elimination.md` item 2), NOT here.

## C4 — `ModConst` (fuse `x = x % C`)  ·  MINOR, likely net-negative now

`s = s % 1000000007` appears in nearly every bench. Already native (`IntBin`
with an immediate second operand); a dedicated `ModConst` would bake the divisor
+ skip the operand decode and the div-zero check (C is known-nonzero).
- **Adds a NEW op** → per `[[vm-dispatch-frontend-regression]]` that risks a
  front-end regression that dwarfs the tiny decode saving on a switch dispatch.
  The plan itself calls it "minor". **Defer until AFTER C2** (computed-goto
  makes op-set size front-end-neutral), or skip.

## C1 — typed operands into member/subscript/builtin READS  ·  LARGELY DONE

Landed as the typed dict / struct / element read ops (`DictLoadInt/Float`,
`LoadStructFieldInt/Float`, `LoadElemInt/Float`). Residual: a boxed builtin
RESULT feeding a typed arith chain still boxes (`40_math_builtins` gate) — a
typed-result builtin variant would remove a box+unbox per hot read. Small.

## C5 — don't re-box a bool comparison feeding a branch  ·  LARGELY DONE

Done via `JumpUnlessIntCmp`/`JumpUnlessFloatCmp` (fused compare+branch) and
`JumpUnlessTrueV`. Residual audit: any `BinOpV`(compare)+`JumpIfFalse` pair that
should be a fused `CmpV`+branch.

---

## Bigger / further-out

- **Superinstruction fusion** — mostly KILLED by a cachegrind study: fusing a
  typed operator's operand reads saved ~1 instr/iter (0.03%), because it only
  trades a well-predicted monomorphic vcall for an equal-length inline branch.
  The per-node dispatch tax can only be removed WHOLESALE (that's C2 / the JIT),
  not by node-level superinstructions. Revisit only with a PMU.
- **Machine-code JIT (x86-64 / arm64)** — the far-future endgame
  (`[[vm-endgame]]`, "no cheating: ops must lower to real machine code"). C2
  threaded dispatch is the last interpreter stop before it; a JIT eliminates
  dispatch entirely by emitting each handler's code inline. Requires the
  AST-free serializable bytecode first.
- **`Instr` shrink** — dropping the 8-byte `node` field (~12% smaller `Instr` →
  hotter instruction stream). This is now folded into the `.myv` / AST-free
  endgame (a fallback op's node → a serializable pool index, or the construct is
  nativized), NOT a standalone perf item. See
  `plans/vm-fallback-elimination.md`.

## Need first: a PMU

The single most valuable enabler is a machine with **hardware performance
counters** (branch-misprediction / DSB / I-cache counters). On WSL2 (no PMU) the
front-end effects that dominate these micro-changes are invisible to cachegrind
(instruction count) and only show as wall-clock deltas whose CAUSE we can't
confirm. Any of C2/C4 (dispatch/layout-sensitive) should ideally be measured on
a bare-metal Linux box with `perf stat`.
