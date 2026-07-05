# Bytecode VM

Status: **Phase 5 in progress** — the register machine runs resolved-local
int/float scalar loops (`while`/counted `for`, fused `ForLoopStep`) natively at
top level, inside function bodies (`do_func_call` hooks `vm_run_chunk`), and
NESTED (nested loops + `if` in a body compile directly into the chunk with
backpatching); array element read/write `a[i]` / `a[i]=v` / `a[i][j]` and a
scalar builtin/call in an expression are native; a flow-free statement runs as a
fallback within an otherwise-native loop, so array-building loops (matrix/sieve)
go native. NOW ALSO: native-first codegen for FUNCTION BODIES + top-level (not
only loop bodies - `gen_stmt` tries the register machine first), `LoadImm` for
constant moves, native for-init, native COMPOUND loop conditions (`while (A &&
B)` -> a compare-branch chain), and an M8 fix so an auto-const-folded operand
(`var N=8; while(i<N)`) specializes (mandelbrot/bit_hash went whole-loop
fallback -> native). Disassembly reads as `i.jmp.ifnot a < b, L` / `load rN,#k`.
**Suite geomean ~0.73x (VM ~1.35x faster than the tree-walker)**; recursion
stays neutral; the whole `-rt` suite passes under both engines. Branch:
`exp-work`.

**Perf-regression note (2026-07-04, user-observed):** the BROAD geomean
(mylang-vs-CPython, the `bench/run.py` verdict) has slowly slipped from ~4.1x to
~3.8x faster than CPython over the last several VM commits. The boxed-tier /
call ops trade the tree-walker's already-tuned paths for not-yet-optimal native
ones
(boxed arg arithmetic through `BinOpV` instead of unboxed `IntBin`, unfused
op sequences, a per-op switch where the tree-walker had one M8 vcall). This is
ACCEPTED for now — the priority is correctness + removing every `node->eval`
fallback first; a second pass reclaims the perf (typed arg lowering, fusing,
dropping the boxed marshalling) once the fallbacks are gone. **Resume perf
tuning after the fallbacks are removed** — do not let the slip grow unbounded.

**Update (2026-07-04): ~3.8x → ~3.7x** — caused by the builtin VALUE-ABI
migration (the num/str/io batches; the only commits since the 3.8x note).
UNLIKE the slips above (VM-path only), this one hits the DEFAULT (tree-walker)
engine, which is what `bench/run.py` measures: the tree-walker now reaches a
migrated builtin through `builtin_v_adapter`, which stack-constructs an 8-slot
`EvalValue` arg buffer and calls `func_v` through a SECOND function-pointer hop
— vs the old single direct call that evaluated the args inline. Benches that
call migrated builtins (40_math_builtins etc.) pay it per call. **Reclaimable:**
on the hot `DirectBuiltinCallExpr` path, call `func_v` directly (build the
buffer in `do_eval`) so the tree-walker skips the adapter hop too, and/or size
the buffer to `n` not 8; the adapter then survives only for the cold
generic-CallExpr / const-eval paths. Because this regresses the *default*
engine (not just opt-in `-vm`), it is a higher-priority reclaim than the earlier
slips.

**THE DIRECTIVE (user, 2026-07-04), see [[vm-endgame]]:** do it ALL (every
statement/expression kind native, ordered by EASE not perf impact); native ops
must fully support `dyn`/general values and NEVER fall back to the tree walker;
the end state removes `Construct*`/`EvalStmt` entirely; and the ops must be
primitive enough to lower to real machine code (x86-64/arm64) later - so an op
may call a runtime function (num_bin_op, a builtin, a Type method) but may NOT
`node->eval()`. The current `EvalStmt`/`EvalToSlot` fallbacks are TEMPORARY
scaffolding. **The big missing piece is a BOXED general-value path** (EvalValue
registers + load/store/binop/call/subscript/make ops over the runtime), the
tier below the typed unboxed fast path, that makes a `dyn` value / string / dict
/ struct / general call run as native ops. See "Boxed general-value path" below.

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
- ~~`01_while_loop` +8.1% instructions, Phase 1~~ — **ERASED in Phase 2.** The
  register machine (native int loop) took it from +8.1% to **−50.3%
  instructions** (cachegrind) / **0.51x wall-clock** — a ~2x *win*, not just
  parity. No open tracked regressions.

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

### Phase 2 — a REGISTER machine over the frame slots (NOT a value stack)
Where the VM starts to *win* — and an architecture decision that supersedes the
original "value stack" plan.

**Why register, not stack.** The tree-walker's scalar paths are already at their
floor (the Option-A cachegrind study: fusing a typed node's operand reads saved
0.03%). A naive **stack machine** re-encodes each node as push/pop — per-op
dispatch ≈ the tree-walker's per-node vcall, *plus* stack traffic — so it would
be neutral-or-worse. Instead: the interpreter already has **frame slots** for
resolved locals, so the VM's **registers ARE the slots**. Operands name slot
indices directly; there is no value stack. Combined with **fused
superinstructions** (one op = a whole statement/condition), the VM does *fewer,
fatter* ops than the tree-walker's per-node dispatch — a real win. Bonus: a
3-address slot-based IR is the right on-ramp to the maintainer's eventual native
x86-64 codegen (slots → registers/memory), so this IR is not throwaway.

- **Step 2.0 — resolved-local INT scalar loop — DONE.** Two ops:
  `IntBin{dst_slot = a <arith> b}` (3-address; a/b are `Operand`s = slot or int
  immediate) and `JumpUnlessIntCmp{a <cmp> b -> target}` (fused compare+branch).
  Codegen (`try_native_int_while`) compiles a `while` whose condition is a leaf
  int compare and whose body is entirely compound-assigns / `++`/`--` of leaf
  int operands into `JumpUnlessIntCmp` + `IntBin`s + a back `Jump` — no
  `LoopBackEdge` (a compilable body has no break/continue/decl). Anything
  unsupported (nested rhs, float, plain assign, global/capture slot, a call,
  ...) falls back to Phase 1 exactly. VM reads/writes slots directly
  (`read_int_operand`/`write_int_slot`, bool slot read as 0/1); div/mod keep the
  zero check with the source `Loc`. **Result: `01_while_loop` −50.3%
  instructions / 0.51x wall-clock (~2x win)** — erased the Phase-1 regression;
  geomean **0.99x**; `53_collatz` (worst wall outlier) +0.00% instrs = confirmed
  noise, no accidental regression. 1303/1303 + 1155/1155; shape test
  `vm: codegen shapes` pins the opcodes.
- **Step 2.1 — nested int expressions + plain assignments — DONE.** A recursive
  `compile_int_expr` lowers an arith/neg `TypedScalarExpr` tree into `IntBin`s
  writing **scratch temp slots** (the register allocator: temps laid out above
  the resolved locals at `[slot_count, slot_count + n_temps)`, reset per
  statement, high-water sizes the frame; `Chunk::n_temps`, `vm_execute` grows
  the frame). Compound assigns take a nested rhs (`s += i*i`); a plain
  `x = <expr>` compiles with a peephole that retargets the last `IntBin` to
  write `x` directly (`x = a*b + 1` → 2 ops). **Bool-safety:** the rhs must be
  `definitely_int` (an arith/neg TSE or int literal — never a bare leaf
  Identifier or a comparison, both of which can be bool), because writing an int
  result into a bool slot would corrupt it; a valid `x = <int>` guarantees x is
  int (bool can't accept int), so it is sound. Compound assign / condition
  operands read a bool slot as 0/1 (arith promotes), so they stay safe.
  **Result:** a nested-expr loop (`s = s + i*i - i`) is **−61.5% instructions**
  (bigger win — more nodes, more dispatch removed); `01_while_loop` still −51.7%
  (no regression); geomean **0.99x**, worst outlier +0.00% instrs (noise).
  1303/1303 + 1155/1155; the shape test now pins native / fallback / nested /
  bool-safe.
- **Step 2.2 — float scalar loops — DONE.** `FloatBin` / `JumpUnlessFloatCmp`
  (operands read as float — an int/bool slot promotes, mirroring
  `eval_float`; div/mod keep the zero check, mod via `fmod`). `Operand` gained a
  `flit` (float immediate). Parallel `compile_float_expr/stmt/cond`;
  `try_native_scalar_while` tries an all-int loop, then an all-float loop (a
  MIXED int/float loop falls back — handled later). Float needs no bool-safety
  (a float destination is never a bool slot), so a plain `f = <float leaf/expr>`
  compiles too. **Result:** a pure-float loop (`s = s + x*x - x`) is **−71.0%
  instructions** (the biggest win yet — `eval_float` has the most per-node
  dispatch); `01_while_loop` still 0.51x; geomean **1.00x**, the four worst
  wall-clock outliers all cachegrind-confirmed +0.00% instrs (noise — the bigger
  `Operand` did NOT regress int loops). 1303/1303 + 1155/1155; shape test pins
  the float path.
- **Step 2.3 — mixed int/float loops — DONE.** `try_native_scalar_while` now
  dispatches the condition and EACH body statement by its own kind (try int,
  then float — a failed int attempt truncates its partial ops before the float
  retry), replacing the old all-int-then-all-float two-attempt structure. So a
  `while (i < N) { s += i*i; f += i*0.5; i++; }` (int counter + float
  accumulator) compiles fully native: **−51.0% instructions**; homogeneous loops
  emit byte-identical bytecode (`01_while_loop` unchanged). 1303/1303 +
  1155/1155; shape test pins the mixed path.
- **Step 2.4 — counted `for` loops (ForRangeStmt), via a FUSED back-edge —
  DONE.** First attempt used GENERIC ops (JumpUnlessIntCmp + an IntBin increment
  + a Jump = 3 dispatches/iter for the counter) and measured **+28%
  instructions** on `02_for_loop` — I wrongly concluded the VM couldn't beat the
  tree-walker's raw-C ForRangeStmt and deferred it. The maintainer pushed back:
  a `for` should at least be on par. The real fix was a **fused
  superinstruction**, `ForLoopStep{i += step; if (i <cmp> bound) loop back}` -
  ONE dispatch for the whole counter, matching ForRangeStmt's inlined C. Codegen
  (`try_native_for_range`): `init` once (fallback EvalStmt - it declares `i`, a
  frame slot), an initial `JumpUnlessIntCmp`, the native body, then the fused
  `ForLoopStep`. **Result: `02_for_loop` +28% → −11.9%** (a *win* even on the
  trivial body `s += i`, because the VM's `IntBin` body is leaner than the
  tree-walker's `body->eval` chain while the counter is now on par); a
  heavy-body `for` is −56%. **This moved the suite geomean 1.00x → 0.89x** -
  many benchmarks have top-level `for`s: `03_int_arith` −71.6%,
  `51_purefunc_fold` −67.8%, `08_func_call` −57.7% instructions (cachegrind);
  the worst wall-clock outliers are +0.0% instrs (noise). 1303/1303 + 1155/1155.
  **Lesson (corrected):** the VM CAN beat an already-optimal tree-walker path -
  but only with a fused superinstruction, not generic ops. Don't conclude
  "can't win" from a naive encoding; try the fused form first.
- **`global`/`capture` operands — deferred (soundness cost > value).** A native
  op reading an *undefined* global can't throw a proper `UndefinedVariableEx`
  without threading the var's name + exact `Loc` onto every `Operand` — real
  complexity for benchmark-zero value (no top-level loop uses a global var). If
  it ever matters, do it when function bodies compile (Phase 4), which needs the
  capture/global machinery anyway.
- **Next steps:** function bodies (Phase 4: calls + per-function chunks) — the
  only remaining step that moves the suite geomean, since the benchmarks' hot
  loops live in functions. The scalar register machine (int/float/nested/mixed
  `while`) is otherwise complete.

### Phase 3 — compact the Instr encoding + confirm statement coverage — **DONE**
- **Instr compaction:** an `Operand`'s int immediate (`lit`) and float immediate
  (`flit`) are mutually exclusive, so they now share a **union** →
  `sizeof(Operand)` 24→16, `sizeof(Instr)` 80→64 (−20%). Perf-neutral now
  (geomean holds 0.89x, top-level chunks are small) but it matters once Phase 4
  compiles many function-body Instrs. (Further compaction — a per-opcode
  variant / operand pool — is possible but deferred; 64 B is fine.)
- **Statement coverage:** the register machine already made assignments/
  compound-assigns/inc-dec native in Phase 2; **decls with an arith/literal
  rvalue** (`var t = i*2`, `var k = 100`) also compile natively (the plain
  assign path treats them as a write to `t`'s frame slot) - verified and pinned
  by a shape test. So the Phase-3 exit ("common assignment/decl native") is met.
- **Residual (documented, not a gap to fix now):** an int **leaf copy**
  (`x = y`, `var t = i`) stays a fallback — a bare `th == i` leaf can be a bool,
  and writing an int into a bool slot would corrupt it; distinguishing needs a
  separate bool `TypeHint`, too invasive for the value. A NATIVE `if` inside a
  loop body (conditionals in loops) is genuinely valuable but is control flow
  (needs the loop to emit directly into the chunk with backpatching, not into a
  temp op vector) — deferred to the long-tail (Phase 5).

### Phase 4 — function bodies run via the VM — **DONE**
Runs function BODIES through the register machine so the −50-70% loop wins land
inside functions, not just at top level. The approach is simpler than the
originally-planned "CALL opcode + VM call stack": a call still goes through
`do_func_call` (the VM reaches it via a fallback EvalStmt for the CallExpr), and
`do_func_call` is **hooked** to run the callee's body via `vm_run_chunk` instead
of `body->eval`. So it reuses ALL of `do_func_call`'s machinery — frame,
param-binding, capture ctx, and the backtrace-recording catch — unchanged; no
separate call stack, no RET opcode.
- **4.0 (refactor):** the dispatch loop is extracted into
  `vm_run_chunk(chunk, ctx)`; `codegen_chunk(block, slot_count)` generalizes
  codegen to any block + frame size. `vm_run_chunk` stops on `Halt` OR an
  in-flight `return` (EvalStmt / LoopBackEdge check `flow==ret`) — a function
  body's `return` (a fallback EvalStmt) sets `flow`, and `do_func_call` reads
  `flow.value` as before. Neutral.
- **4.1 (feature):** `vm_func_chunk(fdecl)` compiles a **scope-free block body**
  and returns it ONLY if it has ≥1 register op (a real native loop); an
  expression body / a body with no native content returns null → tree-walked.
  The chunk is cached **on the FuncDeclStmt** (`vm_chunk`/`vm_chunk_tried`,
  opaque `void*`) so the per-call cost is a field read, not a compile/lookup -
  this is what keeps **recursion neutral** (`fib` −0.08%, `10_recursion_deep`
  +0.45%: their bodies have no native loop → null → tree-walked). `do_func_call`
  (under `g_exec_engine==Vm`) sizes the frame to `frame_size + n_temps` and runs
  `vm_run_chunk`. Chunk storage is a per-run `unordered_map` in vm.cpp (cleared
  each `vm_execute`; node-based so `vm_chunk` pointers stay valid).
- **Backtraces** are preserved for free: an error (native div-zero or a fallback
  statement) propagates out of `vm_run_chunk` → `do_func_call`'s existing catch
  records the frame. Verified byte-identical (differential + a hand check:
  a div-0 in a function loop shows `[0] bad(n) [1] main()`).
- **Result:** `55_float_sum` (a float loop INSIDE a function) −62.0%
  instructions; nested-call functions both go native; **geomean 0.87x**
  (VM ~13% faster); no regression (worst outlier +0.00% instrs = noise).
  1303/1303 + 1155/1155.
- **Deferred:** builtin calls / closures-with-captures / non-scope-free bodies
  stay tree-walked (fine — they're correct, just not native); a loop touching
  arrays/dicts/strings still falls back (its operands aren't scalar - Phase 5).

### Phase 5 — long-tail native coverage, one construct per commit
Replace each remaining fallback with native ops, ordered by bench impact, each
behind the differential harness:
- **array-element READ `a[i]` — DONE.** A `LoadElemInt`/`LoadElemFloat` op reads
  the element into a temp (`compile_int_expr`/`compile_float_expr` recognize a
  Subscript over a local-slot array + an int index operand), then the rest of
  the expression uses that temp - so `s += a[i]`, `a[i]*a[i]`, `a[i+1]` are
  native. The op reads the array slot + index DIRECTLY (no Identifier/Subscript
  vcall), mirroring `Subscript::eval_int/eval_float` (neg-index wrap, bounds
  throw with the right loc, flat ints/floats/bools); a dict / general base falls
  back to `node->eval_int`. Win: a single-loop `s += a[i]` is **−26%** instrs,
  `14_array_subscript` **−7.4%** (its read half; the WRITE half `a[i]=v` still
  falls back - next). Geomean unchanged (few bench loops are single + indexed;
  nested loops still fall back). 1303/1303 + 1155/1155.
- **array-element WRITE `a[i] = v` (assign) — DONE.** `StoreElemInt` /
  `StoreElemFloat`: for a flat mutable int/float array it stores the scalar
  directly with COW (slice clones, aliased non-slice clones its live slices,
  then invalidate the cached hash), mirroring `try_flat_subscript_store`; a
  const/read-only / general / dyn base falls back to `node->eval` (sound - a
  compiled rvalue is side-effect-free, so re-eval is exact). The value ops are
  emitted before the index ops (tree-walker rhs-before-index), so a both-throw
  case (`a[i]=b[j]`, both OOB) reports the same error. `14_array_subscript`
  −7.4% → **−58.4%** (both its loops native now). Verified: alias/slice COW,
  write-in-func, const→NotLValueEx, and `hash(a)==hash(deepclone(a))`.
  Compound `a[i] OP= v` still falls back. 1303/1303 + 1155/1155.
- **native nested loops + `if` inside a loop body — DONE (the geomean mover:
  0.87x -> 0.82x).** The loop codegen was restructured to emit the body
  DIRECTLY into chunk.code (jumps chunk-absolute, no relocation) with
  backpatching + SELF-TRUNCATION on failure, and `compile_scalar_body` now
  recurses into nested `for`/`while` and `if` (native `JumpUnless*Cmp` cond, or
  a fallback `JumpIfFalse` for `if (flag)` / `if (a[i])`) - so arbitrary native
  control flow nests. The big win wasn't the sieve/matrix shapes themselves
  (barely moved - a sieve's inner `a[j]=0` write + its cross-cutting spend, and
  matrix's `c[i][j] += ` needs a COMPOUND array store, still a fallback) but the
  many benchmarks that wrap their work loop in a `for(rep)` amplifier: that
  outer loop's body being a loop meant the whole thing fell back before, so
  `04_float_arith` −80%, `05_mixed_arith`, etc. now go native. A
  `break`/`continue`/`return` or any non-compilable body statement still
  self-truncates the whole nest to a fallback (correct). **Dict-subscript fix:**
  `d[k]`/`d[k]=v` (a dict, th==i) was wrongly compiled to a LoadElem/StoreElem
  that fell back at runtime while STILL computing the index/value as native
  operands - a double-compute that regressed `23_dict_insert` +5.7%. The
  inferencer now stamps `Subscript::base_array` (true only when the base is
  statically an ARRAY); codegen emits LoadElem/StoreElem only then, so a dict
  subscript stays a clean fallback (regression -> +0.0%).
  1303/1303 + 1155/1155; nested/for-while/if/if-else/sieve/matrix/triple-nest
  hand-verified vs the tree-walker.
- **compound array store `a[i] OP= v` — DONE** (correctness/completeness, not a
  bench mover). Added as an `aop` on `StoreElemInt/Float` (`Op::invalid` = plain
  assign, else the arith op) so ONE op read-modify-writes the element with a
  single index eval + one COW; div/mod-by-zero is checked BEFORE any clone,
  matching the tree-walker. Verified `+=`/`-=`/`*=`/`/=`/`%=`, float, aliased +
  slice COW, div-zero, `h[d[i]]+=1` histogram. **No bench uses it** (geomean
  unchanged) - real code (histograms, vector ops) does. NOTE: this does NOT
  unlock `46_matrix_mult` (an earlier note was wrong): matrix uses **2D**
  `a[i][k]` whose base `a[i]` isn't a bare local slot, so it needs
  nested-subscript-base support (next), not the compound store.
- **2-D array READ `a[i][j]` — DONE** (correctness/completeness, NOT a bench
  mover). `compile_array_base` loads the outer `a[i]` (an array-valued element
  of the GENERAL array `a`) into a temp via a new `LoadElemValue` op - a native
  general-element read - then the inner `[j]` is an ordinary `LoadElem` on the
  temp; recurses for 3-D+. Both indices native. READ path only: a 2-D WRITE
  through a temp would COW the temp and never write back, so `a[i][j]=v` stays a
  fallback (store codegen keeps `as_array_slot`). Verified 2-D/3-D sum, matrix
  mult, in-func, float, OOB, 2-D-write fallback. **Did NOT speed up
  `46_matrix_mult`** (+0.0%): its OUTER i-loop falls back on `var row =
  array(n,0)` + `c[i] = row` (array-valued statements, not scalar), so the whole
  nest tree-walks and the native 2-D reads inside never fire. A plain 2-D sum
  (no array building) DOES go native. 1303/1303 + 1155/1155.
- **native builtin/call dispatch in an expression — DONE** (`40_math_builtins`
  −10.6%, geomean 0.82x -> 0.81x). An `EvalToSlot` op evaluates a scalar-result
  CALL (a builtin via the baked DirectBuiltinCallExpr fn pointer, or a user
  function) into a temp, so its result is a native int/float operand and a loop
  body like `s += sqrt(i)` / `s += f(i)` / `a[i] = sqrt(i)` goes native instead
  of falling back wholesale. compile_int_expr/compile_float_expr emit it for a
  CallExpr with scalar `th`. INTERIM: the call still evaluates its own args (the
  builtin ABI takes the unevaluated ExprList), so this still holds a
  `Construct*` - a fully Construct*-free builtin dispatch (args into slots, a
  new builtin ABI) is later work toward the no-fallback end-goal.
- **flow-free EvalStmt within a native loop — DONE (the biggest single-step
  move: 0.81x -> 0.75x).** `compile_scalar_body` used to fall the WHOLE loop
  back if ANY body statement wasn't natively compilable. Now a FLOW-FREE
  statement (an assignment/decl `var row = array(n,0)`, a general store
  `c[i] = row`, a void call `append(a,i)`) runs as a fallback `EvalStmt` WITHIN
  the native loop, so the loop still goes native around it - decoupling "the
  loop is native" from "every statement is native." An `any_native` gate keeps
  an ALL-fallback body on the tree-walker's tight counter (no regression:
  `for(i) d[i]=v` still fully falls back). break/continue/return + a nested
  loop/if that can't compile are NOT flow-free (their flow would escape the
  native counter), so they still fall the whole loop back. This finally moved
  the array-BUILDING benchmarks: `46_matrix_mult` +0.0% -> **−37.7%** (outer
  i-loop with `var row=..`/`c[i]=row` now native, the hot k-loop with native 2-D
  reads), `43_sieve` −1.1% -> **−15.8%**; geomean 0.81x -> 0.75x. INTERIM: the
  array-building statements are still EvalStmt (`Construct*`) - true native
  array CREATION (`array(n,0)`, `[]`) + general-element store OPS are later work
  toward the no-fallback end-goal.
- **bytecode disassembler (`-vd`) — DONE** (`disasm.{h,cpp}`), the bytecode
  analogue of `-s`: "smart assembly" (register slots `rN` - unbounded, they ARE
  the frame slots - immediates `#N`, fused superinstructions like `for.step`).
  A fallback op still carrying a `Construct*` (`eval.stmt`/`eval.slot`/
  `jmp.if.not`/...) renders that node via the SHARED AST decompiler
  (`render_construct_code`), so the constructs still embedded in the bytecode
  are shown. This is the AUDIT TOOL for the no-fallback end-goal (see
  `[[vm-endgame]]`): every `eval.stmt`/`eval.slot` row in a hot loop is a
  `Construct*` still to erase; run it per bench to find wasted cycles (the same
  discipline that caught the AST optimizer bugs once we dumped the tree).
  Step-1 scope: main chunk + block-bodied functions reachable through
  Blocks/function bodies (a function nested in a loop/if body isn't walked yet).
- **general (non-range) `for` loop — DONE (`-vd`-found "Gap A").** The `-vd`
  audit of `44_primes_sqrt` (~1.00x) showed `is_prime`'s whole loop was ONE
  `eval.stmt`: `gen_stmt` only lowered a `ForRangeStmt` (the counted `i<bound`
  shape), so a general `for (init; cond; inc)` (e.g. `f*f <= n`) fell back
  entirely - even though the equivalent `while` was already native.
  `try_native_for` now lowers a general `ForStmt` to the WHILE form (`<init
  once> Lstart: <cond> JmpUnlessCmp; <body>; <inc>; Jump Lstart`), reusing the
  while machinery; wired into `gen_stmt` + `compile_scalar_body` (so a nested
  general for is native too). A general-for micro is **−46.6%**. HONEST: geomean
  UNCHANGED (0.75x) - the bench general-fors (`primes`, `sieve`) ALSO have an
  early `return`/`break` (Gap B) or are already range-fors, so none benefit yet;
  Gap A is the prerequisite, Gap B unlocks them. break/continue/return in the
  body still fall the loop back (correct). 1304/1304 + 1155/1155.
- **loop flow: native `return`/`break`/`continue` — DONE (Gap B; geomean 0.75x
  -> 0.73x).** `break`/`continue` compile to native Jumps backpatched to the
  loop's exit / continue point via a codegen loop-stack (`LoopFrame`; nesting
  handled - a break targets the innermost loop; a for's continue lands on the
  fused `ForLoopStep`, a while's on the cond re-test). `return` needs NO new op:
  it runs as an EvalStmt whose `flow==ret` the vm_run_chunk handler already acts
  on (Phase 4) by stopping the chunk - a return abandons the loop, correct. The
  `any_native` gate was scoped to LOOP bodies only (`is_loop_body`): an `if`
  then/else block that's all-fallback (e.g. `if(n%f==0) return false;`) must
  still compile so the ENCLOSING loop goes native - this is what actually made
  `is_prime` native (`-vd`-verified: the whole loop is native, only the two
  `return`s are eval.stmt). Unlocked `44_primes_sqrt` +0.0% -> **−47.7%** and
  `60_bit_sieve` -> **−51.7%**. This is also the on-ramp to killing C++
  exceptions (`[[vm-endgame]]`): control flow is now VM-level jumps, no C++
  throw. break/continue/return + nested + while + range-for hand-verified;
  1304/1304 + 1155/1155.
- **EvalToSlot restricted to BUILTINS — DONE (fixed the closure regression).**
  `s += c()` (a captured, tree-walked closure) had gone native via EvalToSlot,
  but the closure body isn't native, so the box/unbox around the call was
  overhead the tree-walker's in-place `s += c()` avoids (`11_closure_counter`
  +4.05% instrs). EvalToSlot now fires ONLY for a `DirectBuiltinCallExpr`, not a
  general user call: a builtin is cheap so the loop-nativization outweighs the
  boxing; a beneficial user call (`func f(x)=>x+1`) is already inlined away, and
  a non-inlined one (a closure) is better left to fall back. `11_closure_counter`
  +4.05% -> **+0.0%**, with `40_math_builtins` still −10.6% and `08_func_call`
  still −64% (native via inlining). Geomean holds 0.73x. 1304/1304 + 1155/1155.

### The BOXED general-value path (the zero-fallback / dyn tier) — the big piece

**Tier-1 landed (commit a3985d4):** `LoadConstV` (a scalar/string literal from a
per-Chunk const pool), `MoveV` (alias, = doAssign), `BinOpV` (`clone(a) <op> b`
via num_bin_op — arith/bitwise + string `+`, byte-identical to the tree-walker
incl. the clone-left-operand and the right-operand error loc). `compile_boxed_
expr`/`compile_boxed_stmt` fire after the typed attempts, so a `dyn`/string/bool
scalar ASSIGNMENT (`s = s + "x"`) is native, not an EvalStmt. Guards: only a
decl_type none/dyn, non-const lvalue (a typed-numeric decl coerces, a const
reassign must throw — both left to the tree-walker).

**Also landed:** `CompoundV` (`s += x` — copies the lvalue so a container
mutates in place, = doAssign's compound branch; commit bfc8589); `CmpV` +
`JumpUnlessTrueV` (a dyn/string comparison as a value AND in a condition —
`if (a==b)`, `while (x != none)`, `if (x)` — wired into emit_cond_jumps +
compile_native_if; commit 4920ada); `LogV` (boxed `&&`/`||` — EAGER, since
MyLang's `&&`/`||` don't short-circuit at runtime; unified the arith/cmp/logical
cases into one `emit_boxed_chain` that also handles the TypedScalarExpr form, so
`x>0 && x<20` over a dyn `x` — which specializes to a TypedScalarExpr(logical) —
goes native; commit fa114a6); `LoadGlobalV` / `LoadCaptureV` / `LoadBuiltinV`
(a boxed operand can be a global / captured / builtin value, mirroring
Identifier::do_eval incl. the undefined-global throw; commit 3a93b93). (A boxed
ARITH over a TypedScalarExpr operand, e.g. `var dyn d = i + j` with i/j int, is
handled by emit_boxed_chain too.); `SubscriptV` (a boxed `a[i]` READ - dict /
general-array / string element via the runtime Type::subscript, RValue'd; nested
`d[k1][k2]` works; commit 16f58da); `MemberV` (a boxed `obj.f` / `d.k` READ,
commit 974a837 — done with the prerequisite refactor: the value-read path of the
110-line MemberExpr::do_eval is now a shared `member_read(base, MemberExpr*)`
that the tree-walker AND the VM use; the LValue*/auto-vivify WRITE paths stay in
do_eval; behavior byte-preserved). **`foreach` over a flat int/float array**
(single, non-indexed loop var) now goes native too: the inferencer stamps
`ForeachStmt::elem_th` ONLY for that sound case (a single var over a dict binds
the KEY, also int, so the loop var's own `th` is not enough — the container kind
is checked where the static type is known), and `try_native_foreach` lowers it
to a counted loop — snapshot the container once, `n = ArrLen` (a new op),
`x = c[i]` via the existing `LoadElemInt/Float`, the native body, and the FUSED
`ForLoopStep` back-edge (the unfused compare+i++/jump was perf-NEUTRAL vs the
tree-walker's already-excellent flat-int foreach; the fused step is −64%
instructions). Dict / string / general / tuple / indexed / non-local-array
foreach stay the tree-walker fallback. **Native USER-function calls** landed as
`CallV` (the first half of the call work): a call the inferencer proved a user
FUNCTION (`CallExpr::vm_direct_func`, a Func static type - NOT a struct
constructor, whose `construct_struct` self-evaluates its args, nor a builtin)
and that devirtualized to a global slot evaluates its args into a register run
and calls `vm_call_func` → `do_func_call` with the VALUES (no `node->eval`
of the call). Fires for a call as a scalar/boxed operand or a statement; a
callee not yet defined / reassigned throws the same UndefinedVariableEx /
NotCallableEx.
**Native RETURNS landed** (`ReturnV`): `return <expr>;` in a chunked body was an
EvalStmt (the whole return, incl. its recursive calls, tree-walked). It now
compiles the return EXPRESSION via compile_boxed_expr (so `return f(x)` →
CallV, `return a+b` → BinOpV; a bare `return;` loads none) then sets
flow={ret,value} and STOPS the chunk. So a mutual recursion — whose bodies
chunk via the `if`'s native compare — runs its returns AND recursive calls with
no node->eval. **This also forced a CallV fix:** the handler built a
std::vector<EvalValue> per call (a heap alloc), a ~32% regression on deep
recursion; the args now stay in the caller's frame slots, passed as a `VmArgs`
view (pointer+count) that `do_func_bind_params` binds in place — 1.315x →
1.002x (neutral).

**fib (bench 09) is now fully native too** (was the last recursion on the
fallback — its unrolled return is a ternary): (a) a **ternary VALUE** in
compile_boxed_expr (`cond ? a : b` → compute cond, `JumpUnlessTrueV`, one arm
into a reserved `dst`; each arm's calls become CallV/CachedCallV), and (b)
**`CachedCallV`** — the fib-unroll self-calls are `CachedCallExpr` (per-frame
`PureCache` dedup of the exponential); a plain CallV BYPASSED the cache and made
fib 15.7x SLOWER, so CachedCallV routes through `vm_cached_call` (the cache
lookup given evaluated args is a shared `pure_cache_call` both engines use).
fib$0 body → 0 EvalStmt; bench 09 15.767x → 0.996x (neutral, the cache
dominates).

**Builtin VALUE ABI + `CallBuiltinV` — infrastructure + first batch landed.**
A builtin's original ABI `(ctx, ExprList*)` self-evaluates its arg NODES (a
node->eval even on the VM's EvalToSlot path). The VALUE ABI passes them
pre-evaluated. Dual-ABI (incremental, both engines one impl): `Builtin` gains
`func_v(ctx, ExprList*, const EvalValue* args, size_t n)` — set only for a
migrated READ-ONLY builtin; `exprList` is for error LOCS + arity ONLY (per-arg
locs byte-preserved). `make_const_builtin_v<FV>` registers func_v + a generic
`builtin_v_adapter<FV>` as `func` (the tree-walker evaluates + calls func_v; the
VM calls func_v directly). `CallBuiltinV` copies the register args into a buffer
and calls func_v (stamping the args-loc like DirectBuiltinCallExpr); a builtin
without func_v stays EvalToSlot. **Excluded (keep func_v == null → fallback):**
MUTATING builtins (append/push/pop/insert/erase/intptr — need an lvalue arg) and
AST builtins (defined/type/decltype/typestr/kindstr — need the node). **Still
TODO in this tier:**
- **migrate the remaining read-only builtins** to func_v (mechanical:
  `RValue(elems[i]->eval)` → `args[i]`, keeping the exprList locs; an inner `n`
  in a body becomes `nargs`). **Done so far (~50):** len/str/int/float; all of
  num.cpp.h (abs/min/max/round + the std::math wrappers via `float_func`); all
  of str.cpp.h (split/join/ord/chr/pad/strip/startswith/endswith); all of
  io.cpp.h (print/write/read/...). **Remaining (~40):** arr.cpp.h
  (make_array/top/range/sum/reverse/find/sort/map/filter — the callback ones,
  sort/map/filter/find, need care: the callback arg is a value but the container
  semantics differ),
  dict.cpp.h (keys/values/kvpairs/dict/get/get!), generic.cpp.h (clone/hash),
  reflect.cpp.h (globals/layout/...). **Permanently excluded (func_v stays
  null):** the mutating builtins (append/push/pop/insert/erase/intptr — need an
  lvalue arg) and the AST builtins (defined/type/decltype/typestr/kindstr).
- make-array/dict — see the op list below.

The boxed SCALAR + SUBSCRIPT + MEMBER core is complete: a `dyn`/string
expression over locals/globals/captures/builtins/literals/subscripts/members,
with
assign/compound/comparison/logical, runs with no `node->eval`.

The typed unboxed int/float register machine is only the fast tier. To satisfy
the directive (never fall back, full `dyn` support, `Construct*`-free, machine-
code-lowerable) there must be a SECOND tier: a boxed value machine whose
registers hold `EvalValue`s and whose ops call the runtime directly (no
`node->eval`). This is what makes a `dyn` value, a string/dict/struct op, a
general call, `foreach`, etc. run native. Ops (all operate on EvalValue frame
slots / a value-register file - "unlimited registers"):
- **loads:** `LoadSlotV dst, slot` (RValue of a local), `LoadGlobalV`,
  `LoadCaptureV`, `LoadConstV dst, const#` (a baked literal EvalValue),
  `LoadBuiltinV`. (Replaces the const/ident leaves of an EvalExpr.)
- **stores:** `StoreSlotV slot, src`, `StoreGlobalV`, `StoreElemV arr, idx,
  src` (Type::subscript for-write), `StoreMemberV`.
- **ops:** `BinOpV dst, a, b, Op` (dispatch through `num_bin_op` / the `Type`
  vtable - a runtime call, machine-code-legal), `UnOpV`, `CmpV`.
- **access:** `SubscriptV dst, base, idx`, `MemberV dst, base, name#`, `SliceV`.
- **calls:** `CallV dst, callee, argc` with args pre-evaluated into value
  registers - this REQUIRES evolving the builtin ABI off the unevaluated
  `ExprList` (today builtins take `ExprList*` and eval args themselves; the
  end-state passes a `span<EvalValue>`). Until then a builtin call is the last
  `Construct*` holdout.
- **make:** `MakeArrayV`, `MakeDictV`, `MakeStructV` (from value registers).
- **iterate:** a `foreach` lowers to get-iterator + a native loop over
  ForeachV-step ops.
Control flow already exists (jumps + the planned VM-level throw/catch). The
typed tier and the boxed tier interoperate through the frame slots (a typed op
writes an int slot; a boxed op boxing it reads the same slot as an EvalValue).
**Order by EASE:** boxed binary-op + load/store (kills the scalar `EvalStmt`
first), then subscript/member, then make-array/dict, then `foreach`, then the
builtin-ABI change + `CallV`, then string/struct/dict builtins fall out. Each
step deletes a class of `EvalStmt`/`EvalToSlot` and is gated on the differential
harness + `-vd` (watch the `eval.stmt` count fall toward zero).

- **VM-level exceptions (kill the C++-throw overhead) — DESIGNED, not yet built
  (deliberately NOT big-banged).** `-vd` audit: `42_exceptions` (a loop doing
  200k `throw`/`catch`) is ONE `eval.stmt` - the whole `try/catch` loop
  tree-walks, so every throw is a C++ throw (~1.6µs each: heap alloc + DWARF
  unwind). Loop control flow is already VM-level (Gap B: return/break/continue
  are jumps/flow-stops, no C++ throw); this is the remaining piece. The
  mechanism
  (per the maintainer's "smart assembly, VM-level handler table" vision):
  - `exc` register in vm_run_chunk (the in-flight thrown value) + a runtime
    HANDLER STACK (active try regions' catch-dispatch pc).
  - `EnterTry{catch_dispatch}` / `LeaveTry` push/pop the handler stack.
  - native `Throw{expr}`: `exc = wrap(eval(expr))` (reuse ThrowStmt's struct ->
    ExceptionObject wrap), then JUMP to the enclosing try's catch-dispatch
    (compile-time known for a lexical throw) - NO C++ throw.
  - catch-dispatch: `CatchTest{type, handler}` ops reusing `do_catch`'s matching
    (by struct-type name / built-in name), then bind `catch (T as e)`, else
    re-propagate (jump to the outer handler, or set the error + return).
  - CHUNK-LEVEL C++ boundary (REQUIRED for correctness): a fallback op inside a
    try can still C++-throw (a builtin error, a struct-construction failure in a
    `throw` arg). vm_run_chunk wraps its dispatch in `try{}catch(Exception&)`;
    on
    a C++ throw it consults the handler stack and resumes at the catch-dispatch
    (or rethrows if none). So native throws pay no C++ throw; fallback throws
    are
    still caught, just VM-routed.
  - `finally` (scope-guard flow-suspend/resume), `rethrow`, nested try, and the
    uncaught-propagation BACKTRACE must stay byte-identical (the differential
    harness checks exception type/message/Loc/backtrace) - this is why it's a
    focused MULTI-COMMIT effort, not one drop. Suggested increments: (1) the
    handler stack + chunk-level C++ boundary + `try/catch` compiled to a handler
    region with throw STILL C++ (structure native, behavior identical - a safe
    base); (2) native `Throw` op (removes the C++ throw - the `42_exceptions`
    win); (3) `finally`/`rethrow`/binding; each gated on the differential
    harness
    + `-vd`. See `[[vm-endgame]]`.
- native dict read/insert (unlocks the dict/sieve remainder);
- slice read+write;
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
