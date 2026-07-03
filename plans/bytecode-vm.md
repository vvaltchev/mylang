# Bytecode VM

Status: **Phase 0 landed** (scaffold + both safety pillars; the whole `-rt`
suite passes under both engines). Branch: `exp-work`.

The design + incremental task plan for MyLang's runtime bytecode VM. Read the
CLAUDE.md section "Execution strategy: strip compile-time overhead first, THEN
a bytecode VM" for the philosophy; this file is the *how*, step by step.

The hard constraint from the maintainer: **no big-bang.** We will NOT drop a
large bytecode interpreter and then debug it. Every step is small, independently
committable, keeps the whole language runnable, and is gated by the full
pre-existing functional suite running under BOTH engines. If a step can't be
made that way, it's too big — split it.

## Goals

1. Execute the **already-optimized AST** with a flat bytecode interpreter that
   removes the tree-walker's per-node virtual-dispatch tax. Target 5-10x
   CPython (from today's ~2.7x).
2. Get there **safely and incrementally**, with full test coverage at every
   step, never regressing the tree-walker.
3. Keep observable behavior **byte-identical** to the tree-walker — same
   output, same exceptions, same error locations, same backtraces. The VM is a
   faster engine for the *same* language, proven equal by construction.

## Non-goals (for this arc)

- **Replacing the tree-walker.** It stays forever: it is the parse-time const
  evaluator and the producer of the optimized AST. The VM is a second *runtime*
  engine, selected by `-vm`.
- **A new value/scope/type runtime.** The VM reuses `EvalValue`,
  `EvalContext`/`Frame`/slots, the global/builtin tables, `num_bin_op`, the
  `Type` ops, and every builtin. It changes *dispatch*, not *semantics*.
- **A register machine / JIT / cross-function global bytecode up front.** Start
  with a per-function stack machine; register allocation and any
  whole-program layout are deferred optimizations (see "Deferred").
- **REPL `-vm`.** The script path first; the REPL stays tree-walker until the
  VM is mature (a late phase, if at all).

## Fixed decisions (do not relitigate mid-build)

- **`-vm` opt-in now; flip to default + add `-tw` at the end**, only once the VM
  is at parity and faster on the bench geomean.
- **Own files**: `bytecode.h` (opcodes, `Instr`, `Chunk`, `Program`),
  `codegen.{h,cpp}` (AST -> bytecode lowering), `vm.{h,cpp}` (the executor).
  Never woven into `eval.cpp`. Add the three `.cpp` to the Makefile glob (they
  are real TUs, not `.cpp.h`).
- **The VM consumes the post-optimizer AST.** `-vm` runs `infer_types` +
  `run_optimizers` exactly as the tree-walker does, then lowers `root`.
- **Const-eval stays in the parser, tree-walked.** The VM never touches
  compile-time evaluation.

## The two safety pillars (build these in Phase 0, before any real opcode)

### Pillar 1 — the AST-fallback opcode

Two opcodes, `EVAL_STMT{Construct*}` and `EVAL_EXPR{Construct*}`, whose handler
is literally `node->eval(ctx)` (statement: discard/observe flow; expression:
push the `RValue` onto the value stack). With these, a codegen that emits one
`EVAL_STMT` per top-level statement is a **complete, correct executor of the
entire language on day one** — it *is* the tree-walker, merely driven by a flat
list. Every later phase replaces some fallbacks with native opcodes; anything
not yet lowered still falls back. So the VM is never partially-broken: it is
always 100% correct, progressively faster. This is the single most important
mechanism in the plan.

Composition rule: a fallback `EVAL_EXPR` pushes an `EvalValue`; native ops
operate on the value stack. So mixed expressions work — `native_add(x,
fallback_call())` lowers to `[LOAD x][EVAL_EXPR call][ADD]`. The native/fallback
boundary is always a clean stack contract.

### Pillar 2 — the differential test harness

The tree-walker is the **oracle**. Every `-rt` test runs under both engines and
must match. Because `-rt` tests encode their assertions as "throw if wrong" (a
test passes iff it throws nothing, or exactly the expected exception), running
the suite under the VM and requiring the same pass/fail *is already* a
differential check against every encoded expectation. We strengthen it to also
diff: (a) captured stdout, (b) the thrown exception's type + message + `Loc`,
(c) the rendered backtrace. Mechanically: `check()` (tests.cpp) gains an engine
selector; a new suite pass (e.g. `-rt` runs tree-walker then VM, or a
`g_exec_engine` toggle) executes every test both ways and asserts equality. CI
runs both. From Phase 0 on, "green under both engines" is the exit gate for
*every* commit. `bench/run.py --mylang <bin> --vm` (add the flag) tracks the
speedup as native coverage grows.

Since Phase 0 is fallback-only, both engines are identical by construction, and
each subsequent native lowering is validated against the oracle the moment it
lands. A wrong opcode surfaces as a diff on some existing test, immediately.

## Pillar 3 — the performance gate (never ship a silent regression)

Correctness is not enough: the VM exists to be *faster*, so every step is also
gated on the bench. `bench/run.py --vm --baseline <same binary>` runs the
suite with the current binary under `-vm` (current) and without it (baseline),
so the `cur/base` column is **VM / tree-walker** (<1 == VM faster) and the
geomean is the verdict. Run it (release, `ASSERTS=0`) at the end of every phase.

**The target is "VM ≥ tree-walker at every step," but that is an aspiration,
not a theorem.** The end state is strictly faster by construction (native
replaces fallback, and fallback == tree-walker cost). But an *intermediate*
step can be marginally slower for two principled reasons the structured
tree-walker doesn't pay:
- **boundary marshalling** — moving a value between a fallback op's `EvalValue`
  return and the native value stack;
- **dispatch scaffolding** — a flattened region runs several opcode-switch
  iterations where the tree-walker used one tight C++ loop / virtual call.

So a step that adds a thin native shell over still-fallback internals may not
pay for itself until a later phase makes enough of the hot path native. **That
is allowed when it is the smallest sensible increment and it is flagged.** The
maintainer's rule: take the smallest incremental step, and if that step carries
a temporary scaffolding regression that *makes sense*, keep it and keep
improving the engine — do NOT inflate the step just to avoid a temporary
regression. What is forbidden is an **accidental** regression (unexplained, or
from a mistake). So: understood + tracked = fine; surprising = stop and
root-cause. Record every tracked regression under "Tracked regressions" below
(and in the commit), naming the later phase that must erase it; that phase's
exit gate must show it gone. Phase 0 (pure fallback) measured **geomean 1.00x**
(0.99-1.02x band, noise) — the expected neutral baseline.

**Tracked regressions (temporary, understood, must be erased):**
- **`01_while_loop` +8.1% instructions (cachegrind), Phase 1.** Flattening a
  top-level `while` adds per-iteration opcode dispatch (JumpIfFalse + EvalStmt +
  LoopBackEdge) while the condition and body are still fallbacks calling the
  same `do_eval` — pure scaffolding, no native win yet. It is the *only*
  benchmark with a top-level `while` (the rest are in functions → fallback), so
  the suite geomean stays ~1.01x. **Erased by:** the native-loop work (native
  scalar conditions, Phase 2; native loop body, Phase 3) — once the loop's
  condition+body are native VM ops, the VM does cheaper work than the
  tree-walker's per-node dispatch, and the flattening pays. Phase 2/3's exit
  gate must show `01_while_loop` back to ≤ ~1.0x.

**Concrete thresholds (the boundary cost is small, so a big regression is a
bug, not a tax).** `cur/base` is VM/tree-walker; >1 means the VM is slower.
- **Gate on the GEOMEAN** — it is noise-robust; single benchmarks swing ±2-5%
  on this box from thermal/load alone. Healthy: geomean `cur/base` ≤ ~1.03
  (≤3% slower). >~1.05 (5%) is **stop-and-investigate**.
- A **single benchmark** may sit up to ~5% slower *temporarily* only if flagged
  + tracked AND confirmed real. A wall-clock swing under ~5% on one benchmark
  is inside the noise floor — **confirm with cachegrind** (deterministic
  instruction count; no `perf` on WSL2) before treating it as a regression at
  all. "Is this 5% real?" is answered by instruction count, never by re-running
  the clock.
- **A geomean past ~5%, or a cachegrind-confirmed single-benchmark regression
  approaching double digits (and certainly ~30%), is a RED FLAG, not a boundary
  tax** — halt the phase and root-cause it (per-iteration codegen, a rebuilt
  context/frame per call, an extra hot-path allocation, a tree-walker fast path
  the VM bypassed, an accidental O(n²)). The boundary/scaffolding costs are
  small constant factors; they *cannot* produce a large regression, so a large
  one is always a real defect. It is never "acceptable overhead."

**Design guideline (minimize scaffolding, but don't inflate steps): lower only
as deep as you go native, WHERE that's still the smallest step.** Keeping each
not-yet-native straight-line body as a SINGLE fallback block-eval (the native
driver consuming its `FlowState`, as `WhileStmt::do_eval` does) avoids
marshalling and per-statement dispatch. Prefer it. BUT per the maintainer's
call, don't grow a step just to reach zero regression: Phase 1 flattens the
`while` CFG (JumpIfFalse + a single fallback body + LoopBackEdge) even though,
with a fallback condition+body, that is scaffolding with no native win yet — it
is the smallest step that builds the loop machinery, and its cost is tracked
(see `01_while_loop` above) to be erased when the loop goes native. The bar is:
smallest sensible increment + no *accidental* regression, not zero regression.

## Phase roadmap

Each phase: **what it adds**, the **fallback boundary** (what still uses the
oracle), and the **exit gate** (always includes "full suite green under both
engines"). Rough LoC budgets keep steps honest.

### Phase 0 — scaffolding + both pillars — **DONE**
- New files `bytecode.h` / `codegen.{h,cpp}` / `vm.{h,cpp}`; `-vm` flag in
  `mylang.cpp` (after `run_optimizers`, call `vm_execute(root)` instead of
  `root->eval(nullptr)`).
- Opcode set: `OpCode::EvalStmt`, `OpCode::Halt` (enum named `OpCode`, since
  `Op` is the operator enum). `EvalExpr` deferred to Phase 2 (nothing needs it
  yet — don't add unused ops).
- `vm_execute` builds the root `EvalContext`/`Frame`/`GlobalFuncTable` inline,
  mirroring `Block::do_eval`'s root path (a shared `setup_root_context`
  refactor was judged riskier than leaving the oracle untouched — the
  differential harness proves they agree; factor it later if they drift).
- Codegen: root block -> one `EvalStmt` per statement -> `Halt`. (Function
  calls fully fall back: an `EvalStmt` containing a call runs the callee
  tree-walked.)
- Differential harness wired into `-rt`: the `tests` list reruns under the VM
  (`g_exec_engine`) as a SEPARATE `VM differential: M/K` summary line - the same
  tests a second time, counted once (the headline total is unchanged); both
  must be green to exit 0.
- **Result**: `-vm` runs the whole language via fallback; **1302/1302**
  (tree-walker) + **1155/1155** (VM differential) under both debug+ASan/UBSan
  and release; bench gate `--vm --baseline` **geomean 1.00x** (neutral, as a
  pure-fallback engine must be). Plumbing proven.

### Phase 1 — control-flow flattening, "main" body — **DONE**
The maintainer's "collapse if / collapse loops" step. Conditions and bodies
still fall back.
- Added `Jump{target}`, `JumpIfFalse{cond, target}` (cond via `vm_eval_cond`,
  mirroring the tree-walker's `eval_cond` — the typed-condition fast path is
  preserved), and `LoopBackEdge{cont, break}` (post-body flow dispatch,
  mirroring While/ForStmt::do_eval).
- Codegen flattens **top-level `if` and `while`** (`Codegen` in `codegen.cpp`,
  emit + label-backpatch): `if` -> JumpIfFalse + fallback then/else + Jump;
  `while` -> JumpIfFalse + fallback body + LoopBackEdge. Bodies stay SINGLE
  fallback `EvalStmt`s (one `do_eval`, own scope), so break/continue/nested
  loops are handled by the body's own do_eval + FlowState, which LoopBackEdge
  consumes — provably identical to the tree-walker.
- **`for` is NOT flattened** — ForStmt::do_eval wraps init/cond/inc/body in a
  child EvalContext (loop-variable scope) that a naive flatten would drop;
  While/If run in the passed ctx, so they're safe. `for`/foreach/ForRangeStmt/
  leaf statements stay fallback `EvalStmt`. (Most counted `for`s are
  `ForRangeStmt` anyway.)
- **Result**: 1302/1302 + 1155/1155 (VM differential); a codegen SHAPE test
  (`vm: codegen flattens if/while to jumps`) pins the opcodes. Perf gate:
  geomean **1.01x** with ONE **tracked regression** — `01_while_loop` +8.1%
  instrs (see "Tracked regressions"; the only top-level-`while` benchmark),
  erased when the loop goes native (Phase 2/3). Smallest incremental step per
  the maintainer's call; no accidental regressions.
- Deferred to a later step (Phase 2/3): flatten `for` (needs the loop-var scope
  handled), hoist break/continue to real jumps, `return`-as-jump/epilogue (main
  has no top-level return), foreach/try-catch native.

### Phase 2 — native scalar exprs + the value stack (~500 LoC, 2 sub-steps)
Where the VM starts to *win*.
- **2a (boxed, correctness):** value stack of `EvalValue`. Opcodes:
  `PUSH_LIT`, `LOAD_LOCAL/GLOBAL/CAPTURE/BUILTIN slot`, and boxed binary/unary
  ops that pop, call `num_bin_op`/the `Type` op, push. Lower the `Expr0N` ladder
  + `TypedScalarExpr` + `Identifier` + literals; `EVAL_EXPR` fallback for
  subscript/call/dict/member/string. Removes tree dispatch; still boxed.
- **2b (unboxed, speed):** typed int/float stack ops mirroring
  `TypedScalarExpr::eval_int/eval_float` (no `num_bin_op` promotion, no boxing),
  chosen by the node's `TypeHint`. `JUMP_IF_FALSE` consumes an unboxed int
  condition. This is the M8 payoff, now in a flat loop.
- **Exit**: scalar arithmetic/comparison run native (unboxed) in main; suite
  green both; **first VM bench wins** on scalar loops.

### Phase 3 — native statements (~250 LoC)
- `STORE_LOCAL/GLOBAL/CAPTURE`, compound-assign, decl — reusing `slot_rmw` /
  the `handle_single_expr14` fast paths. `i += 1`, `x = expr`, `var y = expr`.
- **Exit**: common assignment/decl native; suite green both.

### Phase 4 — function calls + call stack (~500 LoC)
The maintainer's "global bytecode / stack / calls in the VM" milestone. Removes
the "callee runs tree-walked" fallback.
- Compile **every** function body to its own `Chunk`. `CALL{callee, argc}` binds
  a `Frame` (reuse `do_func_call`'s param binding), pushes a return address,
  jumps into the callee chunk; `RET` pops and pushes the result.
- Start with **user-function direct calls** (`DirectCallExpr` -> `CALL` to a
  known chunk). Builtin calls, closures/lambdas, and struct construction stay
  `EVAL_EXPR` fallback initially (builtins take *unevaluated* args — keep the
  fallback until a dedicated builtin-call op in Phase 5).
- **Backtraces**: the VM call stack must record `BacktraceFrame`s exactly like
  `do_func_call` (name/params-as-strings + call-site `Loc`); each `Instr`
  carries a `Loc` stamped onto an escaping exception. `format_backtrace` stays
  unchanged. The differential harness's backtrace diff is the gate here.
- **Inherited optimizer wins**: the VM lowers the *optimized* AST, so an
  unrolled recursion is already a nested ternary of `CachedCallExpr`s. Falling
  back `CachedCallExpr` keeps the per-frame cache; lowering the ternary is just
  expressions. The fib-class win survives with zero VM-specific work.
- **Exit**: user calls + recursion run VM-native; suite green both; call-heavy
  benchmarks (fib) VM-native.

### Phase 5 — long-tail native coverage, one construct per commit
Replace each remaining fallback with native ops, ordered by bench impact, each
behind the differential harness:
- subscript/slice read+write (general + flat arrays), the flat-store fast path;
- dict ops (read/insert/default), member/POD-struct field access + direct
  (unboxed) field read;
- builtin calls (native dispatch: evaluate args to the stack, invoke the fn ptr
  — the `DirectBuiltinCallExpr` analogue);
- `foreach` (native iterator state), idlist/multi-assign, `++`/`--`;
- string ops; struct construction / construct-in-place append;
- `try/catch/throw/finally` (VM-level handler table, or keep C++-exception
  propagation through the dispatch loop with a handler stack).
- **Exit per commit**: that construct no longer falls back; suite green both.

### Phase 6 — residual audit + flip the default
- Enumerate any remaining fallbacks; a deliberate residual for rare/cold
  constructs is acceptable (the fallback is *correct*, just not fast) — but
  `log` it, don't hide it.
- When the VM is at full parity AND faster on the bench geomean: flip `-vm` to
  default, add `-tw` for the tree-walker, update every driver, CI runs both.

## Cross-cutting concerns (design once, respect in every phase)

- **Error locations & backtraces are part of "identical."** Fallback ops
  preserve locs (they call `eval()`, which stamps). Native ops carry the source
  `Loc` on the `Instr` and stamp it on throw. The VM call stack mirrors
  `do_func_call`'s `BacktraceFrame` capture. The differential harness diffs
  locs + backtraces on every test — this is non-negotiable for an educational
  language.
- **Exceptions.** `throw`/runtime errors stay C++ exceptions; the VM dispatch
  loop propagates them. `try/catch` can remain a whole-statement fallback for a
  long time (correct, cold). Native handler tables are a Phase-5 item.
- **The exotic optimized nodes** (`InlinedCallExpr`, `DirectCallExpr`,
  `CachedCallExpr`, `ForRangeStmt`, `TypedScalarExpr`, flat-array literals) are
  all covered by fallback from day one, and lowered natively when their phase
  arrives. The VM never needs to understand an optimization it hasn't reached.
- **`-s`/`--vm` interplay**: `-s` already dumps the optimized AST; add an
  optional bytecode disassembly (`-sb`?) so a lowered chunk is inspectable, the
  VM analogue of `-s`. Useful for debugging codegen; a small win, do it when
  Phase 1 produces the first non-trivial chunk.

## Stack vs register machine

Start with a **stack machine**: simplest codegen, simplest VM, easiest to prove
correct against the oracle. The AST is *already* optimal (the whole point of the
compile-time work), so a stack VM over it should already be fast. A register
machine (fewer push/pops, direct slot addressing) is a **deferred** speed step
(Phase 7+) taken only if profiling the stack VM says the stack traffic — not the
work — dominates. Don't pre-optimize the machine model.

## Deferred (revisit after parity)

- Register machine / superinstruction opcodes / computed-goto or
  tail-call-threaded dispatch.
- Whole-program (cross-function) bytecode layout beyond per-function chunks.
- REPL `-vm` support.
- A serialized/persisted bytecode format (there is no reason yet).
- JIT. Out of scope; this is a bytecode *interpreter*.

## Definition of done (per phase and overall)

- **Per phase**: (1) the whole `-rt` suite is green under the tree-walker AND
  the VM (the differential pass), including the stdout/exception/loc/backtrace
  diffs; (2) the **bench gate** (`--vm --baseline <same binary>`, release
  `ASSERTS=0`) shows no unflagged regression vs the tree-walker — a regression
  is either fixed or recorded here as temporary+tracked (Pillar 3); (3) the
  phase's target construct is native (no longer falls back); (4) a
  `repl:`-style / functional test pins any new VM-visible behavior; (5) the
  commit updates this plan's status + CLAUDE.md if the implementation shape
  changed.
- **Overall**: `-vm` at full parity with `-tw`, faster on the bench geomean
  (and no benchmark left behind by a still-tracked regression), default
  flipped, both engines exercised in CI. No un-`log`ged fallbacks.
