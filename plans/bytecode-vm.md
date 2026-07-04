# Bytecode VM

Status: **Phase 4 landed** — the register machine runs resolved-local int/float
scalar loops (`while`/counted `for`, fused `ForLoopStep`) natively, both at top
level AND inside function bodies (`do_func_call` hooks `vm_run_chunk`).
**Suite geomean 0.87x (VM ~13% faster than the tree-walker)**; recursion stays
neutral; Phases 0–3 done; the whole `-rt` suite passes under both engines.
Branch: `exp-work`.

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
- **native nested loops / `if` inside a loop body** (compile the body directly
  into the chunk with backpatching, not into a temp op vector) - the geomean
  MOVER: unlocks `43_sieve` / `56_sieve_bool` / `46_matrix_mult`, whose loops
  nest (they still fall back today: an outer loop whose body is a loop isn't a
  scalar statement, so the whole nest tree-walks);
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
