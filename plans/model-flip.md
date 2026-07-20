# Model flip: NATIVE containers with bytecode ISLANDS (design + staging)

The endgame inversion named in `plans/native-aot.md` ("The endgame INVERSION")
and `[[vm-endgame]]`. This file is the self-contained design + the staged,
each-step-lands-green execution guide, written to survive a context compact.

> Read first: `plans/native-aot.md` (the JIT design, approach A, the native-call
> arc) and `plans/native-call-impl.md` (the v1 native-call machine code — the
> pieces this builds on). This file assumes both.

## The inversion (what "model flip" means)

**Today = BYTECODE with native ISLANDS.** A function body is a bytecode `Chunk`.
`jit_compile_chunk` finds maximal runs of `jit_op_eligible` ops (MIN_RUN=4),
replaces each with an `EnterNative` op pointing at a machine-code fragment, and
(for a fully-native single-entry run) deletes the interpreted originals. The
**interpreter (`vm_dispatch`) is the DRIVER**: it dispatches op-by-op and, when
it reaches an `EnterNative`, runs that fragment, which returns a resume pc; the
interpreter continues from there. So a mixed function pays per-op dispatch on
every non-native op between fragments.

**Endgame = NATIVE with bytecode ISLANDS.** EVERY function becomes ONE
`call`-able native BLOB (a *native container*). Its `jit_op_eligible` regions
are machine code; its un-nativizable regions ("islands") are reached by a
`call vm_exec_block(from_pc, to_pc)` into the interpreter. The **native code is
the DRIVER**; the interpreter is called only per-island. Per-op dispatch on the
island ops collapses to ONE call per island (block-level dispatch, not op-level).

## THE STRATEGIC ARC — the flip is a MILESTONE toward full-native AOT

**This is NOT primarily a short-term perf play — it is the structural framework
for full-native AOT compilation** (maintainer, 2026-07-21). The point is:

- The flip makes EVERY function a native CONTAINER — a `call`-able machine-code
  blob whose un-nativized regions are bytecode ISLANDS. That is the *shape* a
  fully-AOT-compiled program has: native code with a shrinking set of
  interpreter escapes.
- **Bytecode islands become progressively RARER as we give more ops native
  code** — EXACTLY like the AST→VM conversion removed its fallbacks (`EvalStmt`
  / `EvalToSlot` / `JumpIfFalse`) one op at a time until the codegen was no-fail.
  Each op we teach the JIT to emit natively shrinks the islands; in the limit the
  islands vanish and the program is 100% native (full-native AOT). The island
  executor (`vm_exec_block`) is the *general* escape hatch that lets us ship the
  framework NOW and nativize ops incrementally, never blocked on covering
  everything at once.
- So an INDIVIDUAL step's perf (e.g. loop containers measuring marginal — see
  M4b) is secondary; what matters is that the framework is in place and the
  island set is monotonically shrinkable. The end state — every function a
  native blob, calling each other natively (M5), islands → ~zero — is the
  milestone. Judge the arc by "are islands shrinking / is the call graph going
  native", not only by a single milestone's benchmark delta.

Concretely, the flip changes three things:
1. **One `EnterNative` per function** (at pc 0), not one per eligible run — the
   fragment spans the whole body.
2. **Islands call the interpreter** (`vm_exec_block`) instead of the interpreter
   calling fragments. The interpreted island bytecode is KEPT (stored once,
   reached only via the native call — NOT the double-copy anti-pattern; it is
   the actual implementation of those ops).
3. **Native `call <offset>` applies to EVERY function**, not only leaf callees
   whose whole body is fully native (`native_leaf`), because every function is
   now a single native blob.

## Why (the payoff, and why it is the right next arc)

- **Dispatch removal on the ISLAND boundary tier.** Removing per-op dispatch on
  the parts BETWEEN native runs — one `call` per island vs N per-op dispatches.
  Modest per-island, but it is the mechanism that makes the next two possible.
- **Native calls become UNIVERSAL.** The v1 native call (native-call-impl.md)
  only fires caller→leaf-callee when the callee's WHOLE body is fully native.
  With containers, every function is `call`-able, so a caller stays in machine
  code across ALL its work (fib / recursion / higher-order / dict / string
  builders), not just the pure-int leaves. This is where the call-bound tier
  (~15-20% of the suite: 08/09/10/11/12/34/35/63/67) finally gets the JIT.
- **The platform for N7 (unboxing / escape analysis).** A whole-function native
  container is the whole-function native IR an allocation-sinking pass needs (a
  temporary's lifetime provable over the container). N7 is the CPython-killer
  (the value-model gap `my/cpp ~4.6x`); it is unreachable from the per-run
  fragment model. The flip is the prerequisite, not a competitor.

Honest scope (Amdahl, per native-aot.md "The path to 10x"): the flip's own
dispatch win is bounded; its VALUE is universalizing native calls (N6) and
enabling N7. Measure each milestone same-binary JIT off/on; do not overclaim a
dispatch win where the time is in the C++ the island calls.

## What exists — the foundation to build on (do NOT rebuild)

- **Emitter** (`jit.cpp` `struct Emitter`): the byte emitter, the fragment ABI
  `size_t frag(void *slots)` (slots in `rdi`, resume pc in `rax`), N5 register
  cache (r10/r11), `emit_call_prologue`/`epilogue` (save/restore around a
  helper call), `call_relocs` (patch `E8 rel32` post-mmap), `exit_pc(pc)`
  (flush + `mov eax,pc; ret`).
- **Classifiers** (`jit.cpp`): `jit_op_eligible` (a native-emittable op),
  `op_fully_native` (a non-throwing int op — deletable), `op_is_branch` +
  `branch_pc_target` (the audited branch enumeration), `op_run_eligible`
  (`jit_op_eligible || callv_native_ok`), `callv_native_ok` (a native call site).
- **Run analysis + insertion** (`jit_compile_chunk`): maximal `op_run_eligible`
  runs, `deletable` (fully-native single-entry), `EnterNative` insertion, pc
  remap over `visit_pc_fields`.
- **native_leaf + native calls v1** (`plans/native-call-impl.md`): the whole-body
  single-run predicate; `jit_call_setup`/`jit_ret`/`jit_frame_setup/leave`; the
  checked exit (`g_vm_jit_exc` → null → `exit_pc` → `EnterNative` re-raises).
- **Exception signalling** (`vm.cpp`): `g_vm_jit_raise` (a KIND a fragment sets),
  `g_vm_jit_exc` (an owned `RuntimeException` a helper caught), `vm_raise` (frame
  walk + caret from the loc side table), `EnterNative`'s post-return handling of
  both + the `JIT_RET_SENTINEL`/`JIT_RET_BOUNDARY` resume protocol.
- **Interpreter core**: `vm_dispatch(chunk, ctx, act)` (the shared dispatch
  loop), `vm_frame_setup`/`vm_frame_leave`, `VmActivation` + the address-stable
  SEGMENTED slot stack, `g_current_ctx`/`g_current_act` (set at `vm_run` entry,
  save/restored for nested activations).

## The core new pieces (the flip's machinery)

### 1. The container PLAN (compile-time CFG analysis) — M1
Partition a chunk's `[0, n)` into an ordered list of SEGMENTS, each NATIVE (a
maximal `op_run_eligible` run, incl. an already-inserted `EnterNative`) or
ISLAND (a maximal run of un-nativizable ops), split at basic-block LEADERS
(branch targets from `branch_pc_target`, plus pc 0). Plus a `container_ready`
verdict (every op is native-covered → the whole body can be ONE container) and,
for a mixed body, the per-island BLOCKER opcodes (the distinct un-eligible ops —
the actionable "what to nativize next" list). This is the data structure every
later milestone consumes AND the prioritization tool. Surfaced in `-vd`.

### 2. `vm_exec_block` — the generic island executor — M2 ✅ LANDED
`BlockStatus vm_exec_block(EvalContext &ctx, VmActivation &act, const Chunk
&chunk, size_t from_pc, size_t *resume_pc)` runs a single-entry island of
interpreted ops in the CURRENT frame and returns a status:
- **FellThrough** — the island ended at an `ExitBlock` terminator; `*resume_pc`
  = the pc the container continues at (its next block).
- **Returned** — hit a `ReturnV`/`Halt`: the function returns (value in
  `ctx.flow`); the container does its own native return.
- **Raised** — an UNCAUGHT throw: `g_vm_exc_pending` is set; the container's
  raise stub exits so the exception propagates with the caret. An exception
  CAUGHT within the island (its own handler stack) just continues → FellThrough.

**Chosen: option (b) — reuse `vm_dispatch` unchanged + a terminator op** (not
the `.cpp.h` split of option a, which relocates the whole hot switch and is the
*bigger* layout risk to `vm_dispatch` — the exact thing to avoid). Two minimal
touches: (1) `vm_dispatch` gained a `size_t start_pc = 0` param (one line, no
hot-loop effect); (2) a new `ExitBlock` terminator op whose handler stashes the
resume pc and `return`s (structurally identical to how `Halt`/the boundary
sentinels already return). `vm_exec_block` = flip the current frame's `boundary`
bit (so an island `ReturnV`/`Halt`/uncaught-throw HANDS BACK here instead of
popping the frame — the container owns the frame's real return), call
`vm_dispatch(chunk, from_pc)`, restore, classify. It reuses the FULL interpreter
(all handlers, nested calls, the per-frame handler stack) with ZERO duplication.
Pinned by the `vm_exec_block_selftest` `-rt` test (fall-through, an internal
boxed branch taken + not-taken, a return, an uncaught div0 → Raised). GOTCHA:
never hold a `VmCallRec&` across `vm_dispatch` (a nested call can realloc the
records vector) — re-fetch `back_rec()` to restore. NOTE the `BRANCHED(pc)`
multi-exit case (an island branch to a native block) is deferred to **M4**
(the exit-dispatch); an M3 straight-line island is single-exit at its ExitBlock.

### 3. Whole-function container EMISSION — M3 (straight-line) ✅ / M4+ (branches)
Emit ONE fragment spanning the whole body, entered by ONE `EnterNative` at pc 0.
Native ops → machine code (via the existing `emit_op`); an ISLAND → a
`call jit_exec_block(desc, island_pc)` (`emit_island_call`) with the status
dispatch (#4). M3 does the STRAIGHT-LINE case (`jit_try_container`); M4 adds
native branches/loops (fragment-local labels — the existing N2 back-edge
machinery) and the island-exit dispatch, so a native LOOP can iterate in machine
code around an island (the first real win). The container fragment bakes its own
`FuncDescriptor` (like the native-call caller) so `jit_exec_block` reaches the
container's chunk via `desc->vm_chunk`.

### 4. Island-exit STATUS dispatch — M4
After `call vm_exec_block`, `test`/`jXX` on the returned `BlockStatus`:
RAISED → raise stub (`exit_pc`); RETURNED → the sentinel path (ret to
`EnterNative`); FELL_THROUGH → jump to `to_pc`'s native label; BRANCHED(pc) →
route the returned pc → its block's native label (a compare-chain or, for many
targets, a small pc→label jump table). The returned pc is ALWAYS a block leader,
so the routing table has one entry per leader — bounded and known at emit time.

### 5. Checked-return unwind for CONTAINER calls — M5
A native `call` to ANOTHER container (not just a leaf) can throw/recurse. The
`test rax; jnz <unwind>` after every `call` (native-aot.md checked-return): the
callee sets a non-0 status in `rax` on a raise, the caller pops its own record
(`pop_window` frees this frame's `ref_slots`) and returns `rax` — frame-by-frame
native unwind; a frame WITH a handler catches instead. The status channel is
SEPARATE from the value (the result is written to the caller's dst SLOT — the
`vm_frame_leave` convention — so `rax` is free). Recursion needs the growable
native stack (deferred; see M5 notes).

## Milestones (each lands fully green before the next)

Green bar for EVERY milestone: `-rt` (debug + release, 1566x2-class + the VM
differential) + `tests/nested_fuzz.py` (tw==vm==cpython) + the `-nj`/`MYLANG_JIT=0`
same-binary A/B (neutral where no container fired, a measured win where one did)
+ `-vd`/`-vdj` eyeballed (and byte-identical CODE dump where the change is
analysis-only). Commit per milestone.

- **M1 — container-plan analysis + `-vd` dump. NO runtime change. ✅ LANDED.**
  `jit_container_plan(const Chunk&, const JitCtx*)` → `ContainerPlan` (maximal
  NATIVE/ISLAND segments + `container_ready` + counts; blocker opcodes listed by
  the dump, not stored — kept the struct lean; no basic-block leader split yet,
  M3/M4 adds it when emission needs it). `-vd` prints a "container plan" section
  per chunk (READY for the not-a-leaf-yet case, else NOT-ready + each island's pc
  span + its distinct un-nativizable opcodes). Only called on the dump path →
  zero runtime change; off-platform stub returns `{}`. Pinned by the
  `vm_disasm_container_plan` `jit:` test (a mixed dict-build body → NOT ready +
  island; a sub-`MIN_RUN` all-arith body → READY). STEP-2.0 pattern (compute +
  dump, emit later); de-risks the CFG + is the prioritization surface. Computed
  over the POST-jit chunk, so an island is a run of interpreted ops BETWEEN the
  inserted `EnterNative`s (accurate to the runtime structure).

- **M2 — `vm_exec_block` executor (standalone). ✅ LANDED.** Option (b): the
  `ExitBlock` terminator op + a `start_pc` param on `vm_dispatch` + the
  boundary-flip executor (see #2). Tested DIRECTLY from C++ (`vm_exec_block_
  selftest`, vm.cpp) over hand-built islands — fall-through, an internal boxed
  branch (taken + not), a return, an uncaught div0. NOT wired to the emitter yet
  (M3). The only runtime footprint on real programs is the new (never-emitted)
  opcode — a dispatch-table/layout perturbation (the documented front-end
  effect); measure cross-binary in a deep session, its value lands with M3.

- **M3 — whole-function container for the simplest MIXED shape. ✅ LANDED.**
  `jit_try_container` (jit.cpp, tried first in `jit_compile_chunk`) compiles a
  leaf, STRAIGHT-LINE body (no branches/handlers/calls) that is exactly ONE
  contiguous island of simple boxed scalar ops (`op_is_simple_island`: BinOpV/
  CmpV/LogV/UnaryV/MoveV/CompoundV/LoadConstV/CoerceNumV) ending in `ReturnV`
  into a native container: ONE `EnterNative` at pc 0 drives a fragment =
  [native ops as machine code] + `call jit_exec_block(desc, island_pc)` for the
  island (`emit_island_call`: prologue/`call`/epilogue + `test rax; jns` →
  FellThrough continues, RAISED exits to re-raise) + native `ReturnV`. An
  `ExitBlock` is inserted at the island's end; pcs + side tables remapped.
  Proven end to end: `jit_container` `-rt` test (`g_jit_container_calls` bumps -
  the "prove the code ran" rule - a correct result + a throw from the island
  propagating + being catchable); differential + `nested_fuzz` 300 all-agree;
  `-vdj` shows the full sequence. **A straight-line container is a MECHANISM
  PROOF, not a win** (the island runs interpreted either way; the container only
  ADDS EnterNative + call overhead - the win arrives in M4 when a native LOOP
  segment iterates in machine code around the island). So it is gated OFF for
  small bodies (`MIN_CONTAINER_ISLAND = 5`), which - verified - matches NO
  bench/sample (the existing per-run path is byte-identical, zero suite
  regression). The threshold is a "don't regress tiny hot functions" guard, not
  a cost model - M4 brings that.

- **M4a — native BRANCHES/LOOPS in the container. ✅ LANDED.** `jit_try_container`
  now admits native branch ops (`Jump`/`JumpUnlessIntCmp`/`ForLoopStep`/
  `IntAddStep`/`JumpUnlessFloatCmp`) - the loop control - via `emit_branch` +
  `label[]`/`fixups` (fragment-local jumps, the existing N2 back-edge machinery;
  whole body = `begin=0,end=n` so every target is in-container). A back edge may
  target the island START (the `call jit_exec_block`, a valid label) but not an
  island interior (gated). The chunk rebuild remaps branch targets. So a native
  LOOP iterates in machine code around a straight-line boxed island - the loop
  control (test + step + back edge) is native, only the island calls the
  interpreter. A container WITH a loop forms regardless of island size (the win);
  a straight-line one keeps `MIN_CONTAINER_ISLAND`. MEASURED (callgrind, a
  synthetic loop, container vs `-nj` - the per-run path gives no fragments there):
  **−3.4% instructions** - a real but MODEST win, LIMITED by the per-island
  `vm_dispatch` RE-ENTRY overhead (each island call re-enters the full dispatch
  loop, eating most of the saved dispatch). Still gated to ONE island of simple
  boxed ops + no calls, so it matches NO bench/sample (real loops have a
  multi-island init+body or richer island ops) - zero suite impact; a mechanism
  step + the M5 prerequisite (real functions have loops). Test: `jit_container`
  sub-test 3 (a loop container runs + correct); differential + fuzzer.
- **M4b(i) — MULTIPLE islands. ✅ LANDED** (c7ec040). The gate collects every
  contiguous island; the remap generalizes (`remap[p] = 1 + p + #islands ending
  ≤ p`, one ExitBlock per island); the emit/rebuild loop one `call jit_exec_block`
  per island. Handles the common init-MoveV + body-island shape. Still matched no
  bench/sample with the narrow op set (the real blocker was the op set, not the
  count).

- **M4b(ii) — widen the island OP SET. INVESTIGATED + REVERTED (a decisive
  negative result, 2026-07-21).** Since `vm_exec_block` IS the full VM, an island
  can run ANY non-native op except (a) control-transfer branches (the BRANCHED
  exit, not wired) and (b) CALLS (F2 C-stack growth). So `op_islandable` was
  widened to a blacklist (all data ops - `SubscriptV`/`MemberV`/`MoveV`/generic
  `IntBin`/... - islandable; calls/branches/handlers excluded). This DID hit real
  code - `45_gcd` (a `while` loop) containerized - but it **REGRESSED it +17.7%**
  (callgrind: 1816M vs 1543M vs `-nj`, which == the per-run path since gcd's
  loop is sub-`MIN_RUN`). ROOT CAUSE: gcd's loop body is a tiny 3-op island
  (`t=b; b=a%b; a=t`) and the native part is just 2 ops (the condition + back
  edge); the per-island `vm_dispatch` RE-ENTRY per iteration costs FAR more than
  the 2 native dispatches it saves. The interpreted path runs all 5 ops in ONE
  continuous dispatch loop with zero re-entry; the container breaks that and pays
  re-entry every iteration. **A loop container WINS only when native-ops-per-
  iteration ≫ the island re-entry** (break-even ~3-4 native ops; the synthetic
  M4a loop with 3 native ops was only −3.4%). Most real loops have small bodies →
  a LOSS. So the op-set widening was REVERTED (don't ship the gcd regression).
  **CONCLUSION: loop containers are marginal-to-negative for real code** - the
  `vm_dispatch` re-entry per island is a hard floor. The M4 branch + multi-island
  machinery is a PREREQUISITE for M5 (real functions loop), not a standalone win.
  The real payoff is **M5 (native calls)** - a caller staying native across
  calls removes the call dispatch + is not re-entry-bound. Reconsider loop
  containers only with a genuinely leaner island executor (M4b-iv), and even then
  small-body loops stay marginal.

- **M5 — native calls to EVERY function.** A container that makes calls, not just
  leaf-callee calls: the checked-return unwind protocol (#5) + (for recursion)
  the growable native stack. This is the universal-native-call payoff (the
  call-bound tier). Absorbs the #55 v2/v3 arc (throwing/recursive/dyn callees).

- **M6 — delete-originals for containers / `.myv`-readiness.** A fully-container
  function keeps its island bytecode as the SOLE copy (reached only via
  `vm_exec_block`); the per-op interpreted DRIVING is gone. `-vd` of a container
  is the `EnterNative` + its island bytecode (labelled as island-only, reached by
  the container). Confirm the `.myv` image still holds only serializable data
  (bytecode + pools; machine code is never serialized — re-JIT on load).

## NATIVIZE OPS — shrink the islands (the incremental path to full-native)

The maintainer's chosen direction (2026-07-21) after M4b showed loop containers
are marginal: **teach the JIT to emit more ops natively, one at a time, shrinking
the island set toward zero** — the AST→VM-style fallback removal. Each op moved
from "island" to "native" removes an island-op TYPE and grows the fragments; in
the limit the islands vanish (full-native AOT). Independent of the re-entry
ceiling (it applies whether or not containers form). The pattern (approach A):
add the op to `jit_op_eligible` + an `emit_op` case that `call`s a `jit_*` helper
(vm.cpp) running the interpreter's EXACT body; a throwing op catches loc-less
into `g_vm_jit_exc` + returns non-0 (the fragment exits, EnterNative re-raises
with the caret from the loc side table); `pick_cached_slots` disqualifies the
op's memory-accessed slots; a NON-throwing op is also `op_fully_native`
(deletable). **Per-op perf is ~NEUTRAL** (a helper `call` ≈ a dispatch) - the win
is STRUCTURAL + cumulative (see the milestone framing above); measure each
same-binary-A/B to confirm neutral-not-regression, don't expect a delta.

**⛔ VERIFY NATIVE EXECUTION WITH HARD EVIDENCE (the bold rule near the top of
the CLAUDE.md model-flip section).** A nativized op is NOT done until a PER-OP
runtime counter proves the native code RAN. "island-op occurrences → 0" is the
M1 container-plan ELIGIBILITY view (hypothetical) - NOT execution. The
verification here (`g_jit_op_run[op]` bumped in each helper + the
`jit_op_nativized` `jit:` test) caught 2 bugs: (1) **the container gate emitted
MoveV/LoadConstV as ISLANDS** - they're in the stale `op_is_simple_island`
whitelist AND now `op_run_eligible`, and the gate checked island-membership
FIRST, so in a container their helpers were NEVER called - FIXED by checking
`op_run_eligible` FIRST; (2) test cases with const args const-folded the pure
call away - FIXED with `runtime()` args.

**Done (13 ops, all green + ASan/clang-clean + EXECUTION-PROVEN via
`g_jit_op_run` + the `jit_op_nativized` test - real-bench evidence:
62_dict_word_count Subscript=2,000,001, 46_matrix_mult MoveV=217, 47_wordcount
Const=200,000):** MoveV (168, `jit_move`), SubscriptV (87, `jit_subscript`,
throwing-read), LoadBuiltinV (154, `jit_load_builtin`), LoadConstV (112,
`jit_load_const` - bakes the const-pool buffer addr), MemberV (`jit_member`,
throwing-read, bakes the member-key buffer addr - a missing-member caret is
byte-identical), StoreGlobalV-plain (35, `jit_store_global`; the compound
`g OP=`/`g++` stays interpreted). Then steps 7-10:
**LoadLiteralObjV** (27, `jit_load_literal_obj` - materialize a baked literal
via eval_literal_obj, buffer-baking), **ArrLen** (10, `jit_arr_len` - the
foreach snapshot bound, non-throwing), **DictLoadInt/Float** (17,
`jit_dict_load` - typed dict scalar read; throwing, the emitter bakes the KEY
ptr: a member's `&consts[idx]`, a subscript's lea'd `&slot[k]`; the throw-path
caret comes from the loc side table), **MakeClosureV** (35, `jit_make_closure` -
closure create, bakes the program-lifetime FuncDescriptor* value). Per-op perf
~neutral (helper ≈ dispatch) but CUMULATIVELY a real win where they're hot:
62_dict_word_count −5.2% (the first 6 on vs off).

**GOTCHA - N5 STALE-CAPTURE (MakeClosureV, the subtle one).** An op that reads
frame slots the EMITTER CANNOT ENUMERATE - MakeClosureV snapshots its capture
sources from frame MEMORY, but *which* slots is the closure's RUNTIME capture
list - can read a STALE value: an N5 register-cached hot slot (a loop
accumulator the closure captures) hasn't been written back to memory. This was
a real JIT-vs-tree-walker DIVERGENCE (a captured `s` came back non-int → a
`TypeError` under -vm where -tw gave 18), and the differential test suite did
NOT catch it - only the mandated per-op execution test (the CAPTURE shape) did.
Fix: `pick_cached_slots` returns `{}` (no caching for the WHOLE run) when the
run contains such an op. The rule for a FUTURE op: if a helper reads slots the
emit site can't name to `bad()`-disqualify, DISABLE caching for the run. Every
other done op reads only KNOWN slots (target/target2/a_slot), each explicitly
`bad()`.

**Step 11 - the BOXED-ARITH pool (BinOpV / CmpV / CompoundV, DONE).** These
boxed ops' multi-operand data (target + two Operands + aop) can't fit in the
SysV registers a JIT helper call gets, and baking `&code[pc]` is UNSAFE
(jit_compile_chunk REWRITES the code vector - EnterNative insert / drop
originals). The fix is a serializable per-op pool **`Chunk::boxed_ops`**
(bytecode.h): `build_boxed_ops` (codegen.cpp, run post-peephole/specialize,
UNCONDITIONALLY - the interpreter ignores it) copies each op's FINAL operand
data into the pool and stores the index in the op's OTHERWISE-UNUSED `target2`
(verified unused for these ops in the interpreter, visit_use_def, and the
retarget whitelist); the JIT bakes `&ck.boxed_ops[in.target2]` (a stable pool-
buffer address) and `jit_boxed_binop`/`_cmp`/`_compound` (vm.cpp) run the
interpreter's EXACT boxed_operand + vm_num_binop bodies. Throwing (div0/type) ->
g_vm_jit_exc + exit_pc, caret from the loc side table (byte-identical, VERIFIED
vs -tw). `-vd` dumps boxed_ops (labelled `derived` - rebuilt on a .myv load, not
primary data). EXECUTION-PROVEN (jit_op_nativized cases: BinOpV=5/CmpV=5/
CompoundV=5). **The container test's islands SHIFTED to `~dyn` (UnaryV):**
BinOpV/CmpV/CompoundV/MoveV are now NATIVE, so the jit_container test (which used
them as boxed islands) had to switch to an op that is STILL boxed - the model
flip working as intended (islands shrink; UnaryV/LogV/CoerceNumV remain).

**Remaining top blockers (bench tally):** CallBuiltinV (~294 - the big one, but
CALLS: a builtin call, some with callbacks that re-enter vm_dispatch → careful),
Halt (83 - a void-function end; needs the return-none path, and the native_leaf
predicate wants a trailing ReturnV so a Halt-ending body needs thought), the
still-boxed unary/logical islands UnaryV / LogV / CoerceNumV (each could reuse
the boxed_ops-pool pattern OR pass in registers - UnaryV is 1-operand, so it
FITS in registers, no pool needed). GOTCHA: an op needing a CHUNK pool
(LoadConstV/MemberV/LoadLiteralObjV/DictLoad-key/boxed-arith) can't bake
`&chunk` (it dangles - the chunk is stack-built then moved) but CAN bake the
pool's heap BUFFER address (`&vec[idx]`), which a vector move preserves
(VERIFIED sound under ASan); a program-lifetime object POINTER (FuncDescriptor*)
is bakeable as a VALUE directly (MakeClosureV/CallV); a MULTI-operand op whose
data can't fit in registers AND can't bake &code[pc] (rewritten by jit) needs a
serializable per-op pool + a spare Instr field for the index (target2 for the
boxed-arith ops). A NON-throwing op must be op_fully_native too, else it makes an
adjacent fully-native loop non-deletable. A partially-nativizable op
(StoreGlobalV) gates on the instruction (`in.aop == Op::invalid`), not just the
opcode.

## Correctness pillars (unchanged from the JIT arc)
- The `-tw` tree-walker differential ORACLE; `nested_fuzz.py` (tw==vm==cpython);
  the `-nj`/`MYLANG_JIT=0` kill switch = a same-binary A/B lever AND the safety
  backstop (off-platform / disabled → the interpreter runs, unchanged).
- RULE 3 (native NEVER throws through a fragment) is ABSOLUTE: every island call
  and every container call CATCHES into `g_vm_jit_exc` / returns a status; audit
  every emitted `call` against it.
- Layout-offset baking (`FuncDescriptor`/`Chunk`/`LValue`) via `JitLayout`
  probes + `static_assert`s (as today).

## Risks (named)
- **Hot-loop layout** (the documented WSL2 front-end lesson): `vm_exec_block`
  must not regress `vm_dispatch`'s pure-native-loop codegen. The `.cpp.h` split
  (option a) keeps the bounded stop-check out of the unbounded body; measure
  same-binary regardless.
- **C-stack depth** for recursion via container calls (F2, native-aot.md) — the
  growable native stack is M5 material; v1 gates recursion out (a self/mutual
  call = not a leaf, but a container CAN call → recursion re-enters the depth
  question; enforce the `MYLANG_VM_STACK` cap at native-call setup as a
  catchable `StackOverflowEx`).
- **Island re-entrancy**: an island that runs a builtin-with-callback re-enters
  `vm_dispatch` (a nested activation) — save/restore `g_current_ctx`/`g_current_act`
  (the native calls already do this).
- **Codegen technique scaling** (deferred): hand-emitting every op is verbose;
  copy-and-patch (native-aot.md) is the likely path once the op set widens.
  Evaluate when hand-assembly hurts, not before.

## Relationship to the other plans
- Builds ON `plans/native-call-impl.md` (v1 native calls) and `plans/native-aot.md`
  (approach A, the fragment ABI, the exception signalling).
- SUPERSEDES the #55 v2/v3 open items (throwing/recursive/dyn callees) — they
  become container-to-container calls (M5).
- Is the PLATFORM for N7 (unboxing) — the whole-function native IR it needs.
