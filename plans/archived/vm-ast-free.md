# VM: an AST-free, serializable bytecode (free the whole AST in -vm mode)

**Goal:** in `-vm` mode, after codegen, **free the entire code AST** (every
`Construct`) and run purely off the `Chunk` — a flat instruction stream plus
serializable side tables. No `Instr` may hold a `Construct *node`. This is what
makes the bytecode (a) dumpable to a file like CPython's `__pycache__`, (b)
lowerable to machine code (JIT / AOT), and (c) smaller per instruction (the
8-byte `node` field goes → better I-cache). See also `bytecode-vm.md` and
`vm-fallback-elimination.md` (the two converge on this end-state).

## The fundamental problem (why we even need a runtime loc table)

A statically-typed compiled language resolves **every type error at compile
time**, so at runtime there is nothing type-related to attach a source location
to; the only runtime faults left (null deref, div-by-zero) are hardware traps.
MyLang is dynamically typed in its `dyn` part, so `a + b` can be a
**`TypeErrorEx` at RUNTIME**, at essentially any operation. To keep error
quality we must point a caret at that op at runtime → we need `pc → loc` for a
large fraction of ops.

This is a solved problem: it is exactly Java's `.class` **`LineNumberTable`**,
CPython's **`co_positions`**, and native **DWARF line programs** — a `pc → loc`
side table consulted **only on the throw/unwind path**, never on the hot path.
MyLang just needs the table to cover MORE ops than a static language, because
more errors are deferred to runtime. The hot path pays nothing; the error path
pays one O(log n) lookup.

## Status of the loc side table so far

`Chunk::locs` (`{pc → (start,end)}`, sorted, binary-searched by `loc_at`) exists
and already made these ops fully `node`-free (they throw via the table): the
register/loop core (`IntBin`/`FloatBin`/`JumpUnlessIntCmp`/`JumpUnlessFloatCmp`/
`ForLoopStep`), `DictLoadInt/Float`, `SubscriptV`, boxed `BinOpV`/`CompoundV`/
`CmpV`/`LogV`, `LoadGlobalV`, `MemberV` (via a member-key pool), and
`CallV`/`CachedCallV` (via a deferred-loc `do_func_call` param). Remaining
`node` users: the builtin calls, `EmplaceStruct`, and the fallback ops
(`EvalStmt`/`JumpIfFalse` + the element-store fallbacks).

## ⚠ Perf regression to RECOVER here (drop `Instr::node`, Step 5)

**2026-07-08 — TRACKED, ROOT-CAUSED (apples-to-apples).** The reported
`run.py --vm` geomean drift (~**4.5×** → ~**4.2×**) was decomposed by timing the
pre-fallback-work binary (`b0090d7`) vs now (`5c3c6d1`) over the SAME current
bench set, same machine. Two effects, ONLY the second a regression:

- **~half is a BENCH-SET artifact, NOT a regression.** The recently-ADDED
  benches are BELOW the geomean, so they drag it by construction:
  `63_closures` 1.8×, `64_struct_create` 1.6×, `66_dyn_foreach` 1.1–1.9×. The
  **OLD binary itself**
  scores **4.36× EXCLUDING 63–66 but 4.15× WITH them** — a ~0.2× drag with ZERO
  code change. Adding a below-average bench lowers a geomean mechanically; it is
  not slower code.
- **~half is a REAL regression on the DISPATCH-BOUND benches**, confirmed
  DRIFT-CONTROLLED (interleaved `old -vm` vs `cur -vm`). Its SIZE depends on
  scale (startup dilutes it): at bench-default scale ~4–5% (best-of-5), but at
  **scale 10 (loop dominates), release wall-clock `CUR/OLD` geomean = 1.125** —
  `01_while_loop` 1.166×, `03_int_arith` 1.106×, `44_primes_sqrt` 1.135×,
  `60_bit_sieve` 1.096× (best-of-9). The overall `run.py` geomean moves less
  (~7%) because non-dispatch benches are unaffected and the nativized ones
  IMPROVED (`66` 0.57×, sort/reverse). Caution: a serial (non-interleaved) pass
  over-reports — `24_dict_lookup` showed 1.20× serial but ~1.0× interleaved
  (machine drift); always interleave. **This is a real-CPU FRONT-END effect, not
  instruction count — see the PROFILED + FALSIFIED-FIX bullets below.**

- **The VICTIM ops, and why it is NOT op-level.** The four slow benches are
  dispatch-bound int/float loops; their hot ops — **`IntBin`, `FloatBin`,
  `JumpUnlessIntCmp`, `ForLoopStep`, `LoadElemInt`, `StoreElemInt`** — were NOT
  touched this cycle, so no handler got a worse implementation. What changed is
  the `vm_run_chunk` DISPATCH: the session added ~8 new op cases
  (`CheckFuncV`/`MapFilterV`/`DeclConstV`/`ForeachDynInit`/`ForeachDynNext`/…),
  growing the switch's handler CODE. NB: `Instr` SIZE did NOT change (the 8-byte
  `node` was always there), so this is a **code**-side effect.

- **PROFILED (2026-07-08, cachegrind `--branch-sim`, `b0090d7` vs `5c3c6d1`).**
  The regression is REAL and REPRODUCIBLE (consistent across repeated wall-clock
  runs); cachegrind REFUTES the specific *mechanisms* I first guessed
  (I-cache / branch prediction), it does NOT refute the regression. Runs on
  `01_while_loop` (scale 1, 3M iters) and a minimal dispatch loop
  (`s+=i; s%=M` × 1M):
  - `01_while_loop`: I refs 562.16M → **565.16M (+0.53%, ≈ +1 instr/iter)**;
    I1 misses 6081 → 6130 (of 565M — negligible); indirect-branch mispredicts
    9,012,965 → **9,012,711 (CUR fewer)**.
  - minimal loop: I refs 174,335,589 → **174,335,896 (+0.0002%)**; I1 misses
    5855 → 5889; indirect mispredicts 4,007,839 → **4,007,611 (CUR fewer)**.
  So the modeled I-cache and modeled branch prediction are FLAT (I1 misses ~6k,
  not a bottleneck at all; indirect mispredicts marginally *lower* on CUR). What
  IS confirmed is a **+0.53% instruction floor on the while loop (~0 on the for
  loop)** = a compiler **register-allocation / spill** effect of the larger
  `vm_run_chunk` (more cases → more register pressure → an extra spill/reload in
  a hot handler).

  **SMOKING GUN (callgrind `--dump-line` + objdump, prof binaries `LTO=0`
  `OPT=1` `ASSERTS=0`).** On `01_while_loop` (3M iters) `vm_run_chunk` self-Ir is
  **318,000,519 (CUR) vs 315,000,517 (OLD) = +3,000,002 = EXACTLY +1
  instruction/iteration**; on the for-loop `/tmp/disp.my` it is 105,000,185 vs
  105,000,183 (+2 total, ZERO per-iter) — so the regression is confined to the
  while-loop handler path (`JumpUnlessIntCmp`+`IntBin`), not the for-loop
  (`ForLoopStep`) path. Static disassembly of the compiled function:
  `vm_run_chunk` grew from **6403 → 6952 instructions (+549, +8.6%)** and its
  stack-spill `mov`s went **40 → 47 (+7)** — the direct evidence of higher
  register pressure. GCC already emits a `.cold` partition, yet the HOT body
  still grew this much. Causal chain, fully evidenced: ~8 new op cases → +549
  static instr in `vm_run_chunk` → +7 spills → +1 spill executed per while-loop
  iteration → the +0.53% instruction regression.

  **Call-graph confirmation (callgrind, `44_primes_sqrt`):** the register VM is
  as dispatch-bound as expected — `vm_run_chunk` is **~89%** (53% direct + the
  inlined `eval.h`/`evalvalue.h`/`stl_vector.h` slices), `do_func_call` ~2%,
  `LValue::get_value_for_put` ~1.3%. (`09_fib_recursive`'s profile is NOT a
  runtime hot path — its cached recursion runs in ~0.006s, so its 115M-Ir
  profile is compile/startup-dominated, RTTI + interning; a sign fib is
  well-optimized, not a target.)

- **THE REAL SIZE OF THE REGRESSION, and why it is a FRONT-END effect (release
  wall-clock, drift-controlled interleaved best-of-9, scale 10, `-vm`).** The
  cachegrind numbers above are the RELEASE (LTO=1) binaries, so the +0.53%
  instruction count IS the release instruction delta. But the release
  WALL-CLOCK regression is far bigger: **`CUR/OLD` geomean = 1.125** —
  `01_while_loop` 1.166×, `03_int_arith` 1.106×, `44_primes_sqrt` 1.135×,
  `60_bit_sieve` 1.096×. A **0.5% instruction increase producing a ~12.5%
  wall-clock regression is a 24× amplification** — the unmistakable signature of
  a **real-CPU FRONT-END / code-layout effect** (µop-cache/DSB eviction, hot-loop
  code alignment, or real indirect-branch/BTB behavior) that cachegrind CANNOT
  model (it has no DSB, no real-BTB capacity, no OoO port model) and that this
  PMU-less WSL2 CANNOT measure (`perf` hardware counters read `<not supported>`).
  It is NOT instruction count, NOT modeled I-cache, NOT modeled branch
  prediction (all three flat/lower). It is the ~50-target switch dispatch's hot
  loop shifting layout when the op set grew.

- **FALSIFIED FIX — "shrink the hot function" does NOT work (measured, reverted
  2026-07-08).** The cold-handler-split experiment (move the ~10 fat cold ops —
  `MakeArrayV`/`MakeDictV`/`MakeClosureV`/`StructCtorV`/`CallBuiltinLV`/
  `EmplaceStruct`/`CallBuiltinLVElem`/`CheckFuncV`/`MapFilterV`/`DeclConstV` —
  into an `ML_NOINLINE vm_cold_op`) DID shrink `vm_run_chunk` **6952 → 5803
  static instructions (smaller than OLD's 6403)** and kept `-rt` green
  (1355/1355 + 1204/1204). Yet it made the regression **WORSE, not better**:
  release wall-clock `NEW/OLD` geomean **1.168 > CUR/OLD 1.125** (worse on every
  bench), and the prof-binary `vm_run_chunk` self-Ir on `01_while_loop` rose to
  **330M (vs CUR 318M, OLD 315M)**. Lesson: the regression is a **fragile layout
  effect** — perturbing the layout (even to shrink it) reshuffles the hot loop's
  register allocation + code placement UNPREDICTABLY, and here it went the wrong
  way. "Function size" is thus DISPROVEN as the lever; ad-hoc restructuring is a
  gamble, not a fix.

- **FALSIFIED MICRO-OPT — switch → `.rodata` array lookup (audited, measured,
  reverted 2026-07-08).** Hypothesis: the big switches aren't jump-tabled;
  convert them to array lookups (Op/OpCode enums are dense 0-based). AUDIT (gcc
  `-O3` disasm): the HOT dispatches are ALREADY jump tables — `switch(in.op)` →
  `notrack jmp *%rax`; IntBin's `switch(in.aop)` → `cmp; lea table; movslq
  (table,idx,4); jmp *%rdx`. The only convertible ones are the VALUE-mapping
  switches `binop_pmf`/`cmp_pmf` (Op → a `NumBinOp` PMF), which GCC compiles as
  a jump-table-of-constant-loads (an indirect branch just to fetch a constant).
  Converting them to hard-coded `.rodata` tables (`static constexpr NumBinOp
  t[op_count]`, positional — C++17 has NO designated array initializers, a C99
  extension g++/clang/MSVC all reject; verified `.rodata` via `nm` letter `r`,
  portable, `-rt` green) measured **~2-3% SLOWER** on the boxed path
  (`66_dyn_foreach`, best-of-15, `rodata/switch` 1.023-1.027). WHY: for a
  PREDICTABLE dispatch in a tight loop the jump table's indirect branch is
  perfectly predicted (~free) and the `mov $imm` has no load latency, whereas
  the table forces a LOAD with latency on the critical path before the PMF
  indirect-call. (A `.data` constructor-filled table was even worse — an
  immutable `.rodata` table optimizes better than a runtime-filled `.data` one,
  but still lost.) It is CPU-DEPENDENT: the branchless table could win on a
  weak-indirect-predictor CPU or an UNPREDICTABLE op stream; on this strong
  predictor with a 2-op loop the switch wins. Reverted. Lesson: GCC already
  jump-tables the dense switches; hand-conversion only helps if the compiler
  DIDN'T table it (it did) or the dispatch is unpredictable.

- **Candidate fixes — now that "shrink it" is falsified:**
  1. **Computed-goto / direct-threaded dispatch** (Part C2, `&&label` on
     GCC/clang; keep `switch` on MSVC). This is the STANDARD cure for a
     switch-interpreter front-end problem: one indirect branch per op instead of
     a single shared hub, and the compiler can co-locate the hot ops. BUT it is
     ALSO a layout change, so — like the cold-split — it could backfire, and
     without a PMU (DSB-uop / branch-misprediction counters) it cannot be
     validated here. Do NOT land it blind; measure it on a PMU-capable box.
  2. **Drop `Instr::node`** (Step 5 → `Instr` 64→56 bytes). A DATA-side change
     (the instruction stream the tight loop re-reads), independent of the
     handler-code layout. Principled and worth measuring — it may help the
     front-end for a different reason than dispatch layout.
  3. **Get a PMU.** The only way to STOP guessing: a bare-metal Linux or a cloud
     VM that exposes `perf` hardware counters (`idq.dsb_uops`,
     `br_misp_retired.indirect`, `frontend_retired.*`) — then the front-end
     mechanism is directly measurable and a fix can be tuned, not gambled.
  **Action: do NOT keep perturbing the layout blind (the cold-split proved that
  backfires). Either measure on a PMU box, or fold the fix into the principled
  AST-free `Instr::node` drop and re-measure. The bench-set drag (63–66 below
  geomean) is permanent + correct.**

## The plan

### Step 1 — Generalize the loc table to carry per-op AND per-arg locs

Each entry references its argument locs via a flat pool (compact + serializable,
DWARF-style):

```cpp
struct LocEntry { uint32_t pc; Loc start, end; uint32_t arg_off, arg_n; };
std::vector<LocEntry> locs;      // sorted by pc, binary-searched on throw
std::vector<Loc>      arg_locs;  // flat pool; entry's args = [arg_off, +arg_n)
```

`chunk.loc_at(pc)` → the op's caret (arity / whole-call errors);
`chunk.arg_loc(pc, i)` → the i-th argument's caret (per-arg type errors). All
`Loc` + `uint32` — no pointers, fully dumpable.

### Step 2 — Give builtins a loc HANDLE, not an `ExprList` (the key move)

A builtin today reaches into `exprList->elems[i]->start` for a per-arg caret.
Replace that with a small handle it queries only inside a `throw`:

```cpp
struct BuiltinLocs {
    const ExprList *ast;    // tree-walker mode: the AST is retained
    const Chunk   *chunk;   // VM mode: the AST is freed
    uint32_t       pc;
    void arg(size_t i, Loc &s, Loc &e) const {
        if (ast) { s = ast->elems[i]->start; e = ast->elems[i]->end; }
        else       chunk->arg_loc(pc, i, s, e);
    }
    void list(Loc &s, Loc &e) const {                       // arity errors
        if (ast) { s = ast->start; e = ast->end; }
        else       chunk->loc_at(pc, s, e);
    }
};
```

The tree-walker (which keeps its AST) backs it with the `ExprList`; the VM backs
it with `(chunk, pc)`. **One builtin implementation, two loc sources, zero
hot-path cost** — the handle is only touched inside a `throw`.

**Why this is what makes the ~60-site edit tractable.** A first attempt (an
`ArgsLoc {start,end}` param) was reverted because read-only (`func_v`) and
mutating (`func_lv`) builtins BOTH write `exprList->elems[i]->start`, so a sed
to change only `func_v` corrupted the `func_lv` ones, ~37 builtins / ~60 sites.
Routing **every** builtin's loc access through the SAME
`locs.arg(i)` / `locs.list()` handle makes the rewrite uniform and mechanical
(and a `func_lv` builtin keeps the real node SEPARATELY, only for the eval it
still does). "One place for the locs" is precisely what unblocks the edit.

### Step 3 — Remove the last node-EVAL uses

A few `func_lv` builtins (`append`/`push` self-eval) still read arg NODES to
construct-in-place. Make them "rest-native" (args pre-evaluated, like
`insert`/`erase` already are) so they need no node. `EmplaceStruct` already
proves the pattern for the struct-ctor case.

### Step 4 — Nativize the fallback ops (the other half)

`EvalStmt` and `JumpIfFalse` literally call `node->eval()` — the AST cannot be
freed while they exist. Their elimination (see `vm-fallback-elimination.md`) is
the same end-state as this plan, not a detour. This is the current priority.

### Step 5 — Free the code AST in -vm mode, drop `Instr::node`

Once no op and no builtin holds a `Construct *`, free the root AST after codegen
(`-vm` only; tree-walker mode keeps it). Then `Instr` sheds the 8-byte `node`
field.

## Caveats to design in NOW

1. **Freeing "the AST" ≠ freeing everything it OWNS.** Some AST-owned things are
   live runtime DATA, not code: `StructTypeDef`s (a struct instance points at
   its def), interned `UniqueId`s, and the const / member-key / arg-loc pools.
   Those
   must be lifted out (or their owning nodes retained) before the code nodes are
   freed. So it is "free the code Constructs, keep the data descriptors."
2. **The backtrace is already mostly AST-free** — `do_func_call` captures each
   frame's name/params as STRINGS during unwinding (the AST may be gone). Only
   the call-site loc needs the table (already done for `CallV`).
3. **General/user exceptions stay fast.** MyLang uses `FlowState` (not C++
   exceptions) for return/break/continue, so the loc table is touched only by a
   genuine `throw` — one O(log n) lookup on the cold path, nothing on the hot
   path.

## Order of work

Fallback ops first (Step 4 — the real `node->eval` re-dispatch, highest value),
then the builtin loc handle (Steps 1–3, a well-scoped mechanical pass), then
free the AST (Step 5). The builtin loc ABI is NOT a speed win (cold carets
only); its value is serializability + dropping the `node` field.
