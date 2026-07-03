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
- **Result**: `-vm` runs the whole language via fallback; **2457/2457** under
  both engines (debug+ASan/UBSan and release). Plumbing proven.

### Phase 1 — control-flow flattening, "main" body (~250 LoC)
The maintainer's "goto / collapse if / collapse loops" step. Expressions still
fall back.
- Add `JUMP target`, `JUMP_IF_FALSE{cond, target}` (cond evaluated via the
  existing `eval_cond`, reused verbatim — so the typed-condition fast path is
  preserved even in fallback).
- Lower `if`/`while`/`for` in the **main** body to flat jumps + `EVAL_STMT`
  leaves; `break`/`continue` -> jumps to loop end/continue labels; `return` ->
  store result + jump to epilogue (`HALT` for main). No `FlowState` for
  VM-native control flow — jumps replace it.
- `foreach`, `try/catch` stay whole-statement `EVAL_STMT` fallbacks (iterator /
  exception state is fiddly; do them natively much later).
- **Exit**: main's if/while/for/break/continue/return are native jumps; suite
  green both. Heavily exercised immediately — most tests *are* top-level code.

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

- **Per phase**: the whole `-rt` suite is green under the tree-walker AND the
  VM, including the stdout/exception/loc/backtrace diffs; the phase's target
  construct is native (no longer falls back); a `repl:`-style / functional test
  pins any new VM-visible behavior; the commit updates this plan's status +
  CLAUDE.md if the implementation shape changed.
- **Overall**: `-vm` at full parity with `-tw`, faster on the bench geomean,
  default flipped, both engines exercised in CI. No un-`log`ged fallbacks.
