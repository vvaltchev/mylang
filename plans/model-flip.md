# Model flip: NATIVE containers with bytecode ISLANDS (design + staging)

The endgame inversion named in `plans/native-aot.md` ("The endgame INVERSION")
and `[[vm-endgame]]`. This file is the self-contained design + the staged,
each-step-lands-green execution guide, written to survive a context compact.

> Read first: `plans/native-aot.md` (the JIT design, approach A, the native-call
> arc) and `plans/native-call-impl.md` (the v1 native-call machine code — the
> pieces this builds on). This file assumes both.

## The inversion (what "model flip" means)

**Today = BYTECODE with native ISLANDS.** A function body is a bytecode `Chunk`.
`jit_compile_chunk` finds maximal runs of `jit_op_eligible` ops (every run —
the MIN_RUN=4 floor was removed 2026-07-25, see plans/min-run-removal.md),
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

**Done (44 ops, all green + ASan/clang-clean + EXECUTION-PROVEN via
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
closure create, bakes the program-lifetime FuncDescriptor* value). Then the
boxed-arith pool (BinOpV/CmpV/CompoundV), LogV, CoerceNumV, UnaryV (steps 11-14).
And **CallBuiltinV** (the biggest island source, ~294; `jit_call_builtin` - bakes
the builtin_calls pool entry, builds ArgLocs from it, a CALLBACK builtin
re-enters vm_dispatch) - rests on the exception-model change (make
InvalidArgumentEx/InvalidNumberOfArgsEx `DECL_RUNTIME_EX` so `catch
(RuntimeException)` conveys them, no `std::terminate`), and is op_fully_native
(re-raises + carries its own pool caret, so a deleted CallBuiltinV is caret-
correct → no delete-originals regression). See
`plans/callbuiltinv-nativization.md`. Per-op perf ~neutral (helper ≈ dispatch)
but CUMULATIVELY a real win where they're hot: 62_dict_word_count −5.2% (the
first 6 on vs off). And **LoadCaptureV** (`jit_load_capture` - a capture read
`(*ctx->captures)[idx]`, always defined so non-throwing + op_fully_native;
mirrors LoadBuiltinV, uses g_current_ctx like MakeClosureV). Completes the load
family (Builtin/Const/Capture). Its per-op test SURFACED a pre-existing wrong-
result codegen bug (`print(closure(x))` discarded the call result via the
dead-dst peephole rule using STALE liveness after E1 retargeted the call's dst -
fixed in codegen.cpp, its own commit + regression test) - the standing "verify
native EXECUTION with hard evidence" rule paying off: the differential missed it
(the harness JIT-fragmented the shape correctly while the interpreter path was
buggy), only the per-op nativized test (which runs the CLI-equivalent path)
exposed it. And **Halt** (`jit_halt` - a fall-through body's implicit `return
none`, = ReturnV's jit_ret with a none result; op_fully_native; the biggest
single blocker at 83 island occurrences) - see its backlog entry for the
in-VM/boundary paths + why "native_leaf needs a trailing ReturnV" was a
non-issue. And **LoadGlobalV** (`jit_load_global` - a global read; the common
defined case native, the rare undefined case BAILS to the interpreter, N4-style,
so NO exception-model change - see its backlog entry). And **SliceV**
(`jit_slice` - a slice `base[start:end]` via the runtime Type::slice; only
TypeErrorEx is thrown, a RuntimeException -> re-raise; the jit_container test's
island moved to MakeArrayV - see its backlog entry). And **StoreElemValue**
(`jit_store_elem_value` - the UNIVERSAL subscript store `a[i]=v`, ANY base incl.
global/capture via a `kind` arg; an undefined-global base bails, a subscript
throw re-raises - see its backlog entry). And **StoreMemberV** (`jit_store_member`
- a struct field store `s.f=v` via vm_member_store, member key + carets from the
baked member_keys pool - see its backlog entry). And the **nested-chain stores**
(StoreElem2V/StoreElemChainV/StoreLValueChainV) + **StoreCaptureV** (plain
`cap=v`, the capture twin of StoreGlobalV) - see their backlog entries. And
**AppendV** (`jit_append` - `append(a,x)`/`push(a,x)`, the never-throwing
arr_append_fast fast path + the vm_call_builtin_lv_rest fallback; needed the
CannotChangeConstEx -> RuntimeException change - see its backlog entry). And
**CallBuiltinLV** (`jit_call_builtin_lv` - the OTHER mutating lvalue-ABI
builtins: pop/insert/erase/sort/reverse/intptr. Forms arg0 from kind+slot,
calls vm_call_builtin_lv_rest (rest_base >= 0) or func_lv with an empty rest
(rest_base == -1); every throw is a RuntimeException -> g_vm_jit_exc; NOT
op_fully_native, NOT cached. Completes the lvalue-builtin family for a slotted
arg0). And **CallBuiltinLVElem / CallBuiltinLVMember** (`jit_call_builtin_lv_elem`
/ `_member` - the subscript (`append(a[i], x)`) and struct-member
(`append(s.f, x)`) arg0 variants: form the base by kind+slot, derive the
element LValue* via the runtime Type::subscript / the boxed field LValue* via
vm_member_lvalue, then func_lv rest-native. The `run_base` holds the value args
(elem: run[0] index + run[1..] values; member: run[0..] values). Every throw a
RuntimeException -> g_vm_jit_exc (arg0's caret if loc-less); NOT op_fully_native,
NOT cached. Completes the WHOLE lvalue-builtin family). And **MakeArrayV**
(`jit_make_array` - an array LITERAL `[a, b, ...]` via the shared
build_array_from_values over the element run, per the ArrHint in `target2`; the
build has NO error path (a mixed literal just goes general), so it never throws
-> **op_fully_native** - a per-iteration-array loop now DELETES its originals to
ONE `enter.nat` and becomes a `native_leaf` - see its backlog entry). And
**MakeDictV** (`jit_make_dict` - a dict LITERAL via the shared
build_dict_from_pairs over the interleaved key/value run; unlike MakeArrayV it
CAN throw - the key freeze HASHES each key, so a dyn-laundered FUNC key raises
TypeErrorEx - so it re-raises and is NOT op_fully_native. Nativizing it exposed
and FIXED a pre-existing VM caret bug - see its backlog entry). And the **STRUCT
BUILDS** - `StructCtorV` (`jit_struct_ctor` - a POD `P(x,y)` from its field run,
incl. the H1 dst-slot reuse), `StructCtorBoxedV` (`jit_struct_ctor_boxed` - a
non-POD `B(a,x)`, whose field-coerce throw carries the offending arg's POOLED
caret that vm_raise's empty-loc-only stamp preserves) and `MakeStructArrayV`
(`jit_make_struct_array` - the fused flat `array<PodStruct>` literal). All three
throw only TypeErrorEx (a RuntimeException) from `coerce_struct_field`, so no
exception-model change; none is op_fully_native - see their backlog entry. And
the **FOREACH ELEMENT/FIELD LOADS** - `LoadElemValue`, `LoadElemBool`, `StrLen`,
`LoadStrChar`, `LoadStructFieldInt`, `LoadStructFieldFloat`, `LoadStructElemV`:
each the interpreter's body, with the INDEX materialized cache-aware into a
register BEFORE the call (so an N5-pinned foreach counter is read from its
REGISTER, and the counter stays cacheable). All op_fully_native except
LoadElemValue (it bounds-checks) - so a whole foreach body now DELETES its
originals to one `enter.nat`. See their backlog entry for the shape traps. And
**JumpUnlessTrueV** - the BOXED condition BRANCH, the first non-arithmetic op to
join `op_is_branch`/`emit_branch`: `jit_is_true` evaluates the condition (a
TRI-STATE 1/0/-1, since is_true CAN throw) and the FRAGMENT jumps. This was the
top blocker - an un-nativized branch SPLIT the run, so a loop body holding one
left its back edge interpreted; nativizing it made the previous batch's loads run
on EVERY iteration (LoadElemBool 1 -> 100 for a 100-element array). It also
exposed and FIXED a **JIT std::terminate in LogV** - see its backlog entry.
And the **ITERATOR ops (DictIterInit/DictIterNext + ForeachDynInit/
ForeachDynNext, 2026-07-25)** - a dict foreach's live iterator and the dyn
foreach's runtime-dispatching one. The interpreter handlers were REFACTORED
into shared bodies (`vm_dict_iter_init/next_body`,
`vm_foreach_dyn_init/next_body`, vm.cpp) that the jit_* twins also run, so the
engines cannot drift; the per-loop STATE lives on the ACTIVATION as watermarked
slices (act.dict_iters/dyn_iters, record diter_base/dyiter_base + iter_id) and
the helpers reach it via `g_vm_act` like jit_ret, computing the address FRESH
per call (the vectors realloc when a nested call pushes more slices). The Next
pair joined `op_is_branch`/`emit_branch` (the JumpUnlessTrueV shape):
DictIterNext returns 1/0 (never throws - jump to end_pc on 0), ForeachDynNext
is the tri-state 1/0/-1 (the strict N-var unpack throws). ForeachDynInit bakes
`&chunk.unpack_targets[idx]` (a pool-element address) and throws on a
non-container; its + Next's throws go LOC-LESS through the nullable-chunk
`vm_throw_unpack_*` -> the side-table caret (err-loc tests pin vm == nj == tw).
**DictIter* are op_fully_native** (proven dict, frame-slot binds - never
throw), so a dict foreach with an int body DELETES to ONE `enter.nat` with the
accumulator N5-pinned across the whole loop (verified in -vdj: init call, next
at the loop head, `r10 += slot` body, native back edge). N5: DictIterNext's
k/v slots bad()'d; ForeachDynNext writes POOL-listed slots the emitter can't
enumerate AND is a branch (no barrier possible) -> cache nothing for the run.
SHAPE TRAP (the per-op counter caught it): `f(runtime({..}))` makes the
container DYN -> ForeachDyn, so the DictIter test must pass a PROVEN dict
(runtime() wrapping a VALUE inside the literal). Measured (callgrind Ir):
26_dict_iterate **0.952x**, 74_dyn_foreach_kv **0.951x**, 66_dyn_foreach
0.983x; controls neutral (62_dict_word_count's hot loop is an array-foreach +
dict store, not a dict iteration - hence 1.0000).

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

**Steps 12-14 - the LAST simple islands (LogV / CoerceNumV / UnaryV, DONE).**
LogV (eager `&&`/`||`) + UnaryV (`-`/`~`/`!`/`+` on a dyn) reuse the boxed_ops
pool (build_boxed_ops recognizes them; LogV is 2-operand, UnaryV 1-operand with
b unused); LogV never throws (op_fully_native), UnaryV throws on `-str`/`~str`
(caret from the loc side table - a PRE-EXISTING VM-vs-tw col difference, the JIT
matches the VM interpreter). CoerceNumV (typed numeric coerce of a dyn) fits in
registers (dst + src_slot + is_float flag; target2 is the flag, not free for a
pool index) - throws on a bad narrow. With these, EVERY op in
`op_is_simple_island` (BinOpV/CmpV/LogV/UnaryV/MoveV/CompoundV/LoadConstV/
CoerceNumV) is now op_run_eligible - the container gate's simple-scalar niche is
FULLY nativized. **The jit_container test's island shifted to a SLICE `a[i:j]`
(SliceV)** - the only simple boxed op left that is NOT jit_op_eligible; added to
`op_is_simple_island` (verified perf-safe: NO bench/sample forms a SliceV
container - real slice code is inline in main, not a called leaf). LESSON
RE-LEARNED (cost ~an hour): a container-test fn's call MUST use `runtime()` args
or the auto-pure call is FOLDED at compile time and the fn never runs (its
container commits but is dead) - `assert(lc("hi",5)==...)` folded to
`assert(true)`, CC stayed 0. The vm_exec_block MECHANISM is also independently
unit-tested (vm_exec_block_selftest).

**THE NATIVIZATION ROADMAP (maintainer-approved 2026-07-25, "I wanna do all of
that").** The fresh island tally over ALL benches (`-vd` container plans,
post-iterator-ops) and the agreed order. Discipline per step: read `-vdj`
FIRST (does the disassembly match expectations?), prove execution with the
per-op counter, measure with CALLGRIND instruction counts (wall-clock is not
the criterion), stop and ask on anything very surprising.

Tally: IntBin(throwing arms) 16, CallV 13, CachedCallV 12, exception family
~15 (PushHandler 8, CatchTest/Reraise 4, Throw 3, PopHandler/SetPend/
EndFinally), FloatBin(div) 5, CallValueV 4, LoadMemberInt/Float 3,
StructFieldAddInt 2, ForStepElemInt 2, EmplaceStruct 2, IncDecCheckedV 2,
DeclConstV 2, CheckFuncV+MapFilterV 2, UnpackElem* 2, MultiUnpackV 1.

The order:
1. **Throwing IntBin arms** (div/mod/reg-shift-by-generic, 16) - **DONE
   (2026-07-25).** div/mod emit inline `test rcx; raise_unless(JR_DIV0);
   cqo; idiv` (quotient RAX / remainder RDX); the shift arms share the
   NEW `emit_reg_shift` core with the IntShlRR/IntShrRR reg branch
   (negative -> JR_NEG_SHIFT, >= 64 saturates, D3 /4 shl, /7 sar,
   /5 shr for `>>>`), so the two cannot drift. JR_DIV0 joined
   JitRaiseKind; op_fully_native gates IntBin on aop (raise arms keep
   their originals - side-table caret; NOTE the nested aop switch must be
   its OWN case, not mid-fall-through-chain). INT64_MIN/-1 traps in idiv
   = EXACTLY the interpreter's pre-existing UB (UBSan-abort/SIGFPE both
   engines - a language wart to raise with the maintainer separately;
   IntModRI's "-fwrapv defines it" comment is wrong). ⛔ NEW EMIT GOTCHA:
   **`e.bump_op()` CLOBBERS RAX** (movabs rax, <counter>; inc [rax]) -
   it must be emitted BEFORE the operand loads; placing it after
   load_operand wiped the dividend (a wrong 16/3 the differential caught,
   root-caused in one -vdj read). Edge semantics verified byte-identical
   jit/nj/tw (trunc div, sat shifts 63/64/100, catchable div0/negshift,
   uncaught div0 caret). MEASURED (callgrind Ir, matched releases): the
   one div/mod island was SPLITTING whole hot loops - 53_collatz
   **0.194x**, 61_popcount **0.181x**, 59_bit_hash 0.276x,
   44_primes_sqrt 0.297x, 03_int_arith 0.310x, 60_bit_sieve 0.498x,
   45_gcd 0.631x; controls 1.0000. (collatz's plan: "NOT ready - 1
   island: IntBin" -> "READY - whole body native".)
2. **FloatBin div** (5) - **DONE (2026-07-25).** Inline divsd behind a
   sign-stripped divisor BITS test (`movq rax, xmm1; shl rax, 1; jnz` -
   zero iff +-0.0, exactly TypeFloat::div's fpclassify FP_ZERO; NaN/inf/
   denormals divide; a ucomisd test was rejected - unordered sets ZF, a
   bare `je` would wrongly raise on NaN) -> JR_DIV0 raise. Float MOD
   stays interpreted (fmod libm call, 0 bench occurrences - do when it
   shows up). Decoder gained movq r64,xmm / shift-by-1 / xmm rm-operand
   rendering for the SSE reg-reg forms. MEASURED: 04_float_arith
   **0.274x**, 55_float_sum **0.389x** (the div island split those whole
   loops), 54_mandelbrot 0.977 (its divs are per-pixel setup; the inner
   loop was already native); controls 1.0000.
3. **LoadMemberInt/Float** (3) - **DONE (2026-07-25).** The H1 typed
   standalone member read `p.x` via jit_load_member; the interpreter's
   two handlers were REFACTORED into the shared `vm_load_member_scalar`
   (POD byte fast path + member_read_core fallback, whose loc-less get<>
   throw gets the member caret) that the helper also runs - no drift.
   Bakes &chunk.member_keys[idx]; NOT op_fully_native (the fallback
   throws). Test-shape note: a standalone LOCAL `p` (a foreach-array
   element is LoadStructFieldInt instead) with a NON-accumulator field
   use (`s += p.x` fuses into StructFieldAddInt). MEASURED:
   64_struct_create **0.966x** (its 3 islands gone); 65_struct_field_sum
   unchanged (its islands are step 4's); controls 1.0000.
4. **The 65_struct_field_sum trio** - **DONE (2026-07-25).**
   StructFieldAddInt: the field read via jit_struct_field_add_int
   (vm_struct_field_int, never throws -> op_fully_native); the ADD + dst
   write stay IN the fragment so the accumulator remains N5-cacheable.
   ForStepElemInt (a branch): the base GATE runs BEFORE the counter step
   (a bail re-runs the WHOLE op - a post-step bail would DOUBLE-STEP),
   then step/test/the shared flat tails/jump. EmplaceStruct: helper +
   baked &emplace_sites[idx] (kind-formed arg0 like the interpreter;
   N5 = mark_barrier, run length lives in the pool).
   **PLUS a REAL pre-existing JIT bug found+fixed: a NEGATIVE index in
   the native element reads (LoadElemInt/Float, JumpUnlessElemInt)
   RAISED OOB where both interpreters WRAP (`a[-1]`)** - fixed in the
   shared emit_elem_bounds_or_wrap with the wrap on the COLD side (the
   first hot-path version cost 18_foreach_array +3.3%; the final hot
   path is the ORIGINAL single unsigned compare, 1.0000x). Pinned by
   the "negative array index wraps" test. SHAPE TRAPS (counter-caught):
   StructFieldAddInt fires on the FOREACH struct chain `s = (s + p.x +
   p.y) % k`, not `for i: s = s + a[i].x` (boxed); ForStepElemInt needs
   an IntAddModRI body (`s = (s + x) % k`) - a plain `s += a[i]` is
   claimed by IntAddStep. MEASURED: 65_struct_field_sum **0.604x**,
   58_structs **0.896x**, 68_nested 0.997; 18_foreach_array/43_sieve/
   01_while_loop 1.0000.
5. **Tail singles** - first batch **DONE (2026-07-25)**: JumpIfNotNoneV
   (inline none-tag compare vs the NEW JitLayout t_none; op_fully_native -
   a whole coalesce loop deletes to one enter.nat), CmpFloatV (inline
   swapped ucomisd + setcc, the CmpIntV shape; ordering compares only,
   setcc opcode = near_op ^ 1 + 0x10), DeclConstV + DefinedGlobalV
   (trivial never-throwing helpers, op_fully_native), ThrowRuntimeV (an
   unconditional native EXIT - its exception mix includes NON-Runtime
   ones that can't ride g_vm_jit_exc, so the interpreter re-runs the
   side-effect-free throw op; the value is run-shape, ops before it stay
   fused). The container test's island source HOPPED AGAIN: DeclConstV ->
   CheckFuncV/MapFilterV (discarded `map(...)` statements); probe over
   bench+samples = ZERO containers (exit.block grep). Cross-binary
   controls showed +0.8-1.4% on UNTOUCHED paths incl. `-nr` parse-only -
   byte-identical bytecode, so it is LTO/inlining drift (the documented
   cross-binary noise class), not a change cost.
   Second batch **DONE (2026-07-25)**: the STRICT-unpack family -
   UnpackElemInt/Float/Value/Targets via ONE jit_unpack_elem (n_kind =
   N | kind << 8; the Targets variant bakes &unpack_targets[idx]) +
   MultiUnpackV via jit_multi_unpack (targets/coerce pool bakes; the
   compound aop rides an arg) - both over SHARED bodies
   (vm_unpack_elem_body / vm_multi_unpack_body) the interpreter handlers
   also run; vm_throw_multi_unpack_len went nullable-chunk like the
   unpack throwers. N5: the consecutive-run variants disqualify
   [target, target+N) precisely (N = b_lit, enumerable); Targets/Multi
   mark_barrier. MEASURED: 73_multi_unpack **0.739x**,
   20_foreach_unpack 0.988x, 22_multi_assign/controls 1.000;
   **75_indexed_unpack +1.1% same-binary** (jit-effect 0.899 -> 0.909:
   the merged run's helper protocol + some lost pinning trade slightly
   negative on that shape) - a follow-up lean-up candidate, net across
   the family strongly positive. Test-shape note: `var a, b = <dyn>` is
   a compile DynRequiredEx - the MultiUnpackV cases use typed int
   targets (which also exercises the unpack_coerce path).
   Third batch **DONE (2026-07-25)** - the CHECKED INC-DEC family:
   IncDecCheckedV (shared vm_incdec_scalar_body), IncDecElemCheckedV /
   IncDecMemberCheckedV (helpers over vm_incdec_elem/member with baked
   &incdec_sites[idx] - the POOLED dual carets survive the re-raise),
   IncDecChainV (vm_incdec_chain_op SPLIT into root-forming +
   vm_incdec_chain_core, the core shared with jit_incdec_chain; bakes
   &incdec_chains[idx] + the member_keys BUFFER, r9 = the 6th arg). An
   undefined-GLOBAL base/root BAILS in every helper (return 1, no exc -
   UndefinedVariableEx is not conveyable; the interpreter re-runs and
   throws, the LoadGlobalV pattern).
   **PLUS a pre-existing BARRIER-EXIT hazard fixed in the emit loop:** a
   barrier'd op that THROWS exits via exit_pc BEFORE the loop's reload,
   and exit_pc's flush wrote the PRE-CALL register values over slots the
   helper had already modified (reachable for a throwing CallBuiltinV
   callback / MultiUnpackV compound whose written target was N5-cached -
   the interpreter keeps the partial write, the JIT clobbered it back).
   Fix: the cache is EMPTIED across a barrier'd op's emission (flush
   first, so memory is current; the op's exits then flush nothing) and
   restored + reloaded after.
   MEASURED: the only 2 bench IncDec islands (11_closure_counter /
   63_closures, a capture `start++`) - 1.0000x (see the note below);
   42_exceptions / 01_while_loop 1.0000.
6. **DE-HELPERIZE the trivial-value ops** - step 6a **DONE (2026-07-25,
   efa1368)**: MoveV (inline 24-byte payload + Type* copy behind two
   `jae` ref checks - src's runtime type, and dst's current value only
   when ref-listed; either reference -> the original helper), LoadConstV
   (a TRIVIAL const's bytes are EMIT-TIME constants - 3 movabs+store
   pairs + the type, ZERO loads/calls; string consts keep the helper),
   LoadBuiltinV (same emit-time shape). New primitive:
   emit_ref_check_jae. ⛔ THE DIFFERENTIAL CAUGHT A REAL BUG in v1:
   **not every builtins-table value is a trivial Builtin - `argv` is an
   ARRAY in that table**; bitwise-copying it skipped the retain and the
   next release freed it under the static table (ASan UAF at exit on
   every argv-reading bench). A reference table value keeps the helper.
   MEASURED: 22_multi_assign **0.570x** (scalar-spread MoveVs);
   47_wordcount 0.990x (its hot const is a string, helper by design);
   fib/while +0.8-1.5% = the documented cross-binary LTO drift
   (byte-identical images; parse-only alone shifts +0.8%).
   Step 6b **DONE (2026-07-25)**: LoadCaptureV + plain StoreGlobalV /
   StoreCaptureV inline via the probed ctx chain (jit_addr_current_ctx +
   EvalContext::captures/::gfuncs + GlobalFuncTable::slots/::defined;
   walked into r9 by emit_ctx_chain_r9, r9-base loads/stores via the new
   load_r9b/store_r9b - rm=001 + REX.B, no SIB). The STORES' src gate is
   the 3..t_str-1 RANGE (emit_store_src_gate): their helper runs
   RValue(*src), so the pseudo types t_lval/t_undefid AND rare t_none go
   to the helper; the dst current value is ALWAYS ref-checked (globals/
   captures hold anything); the global store writes defined[gslot]=1
   inline (byte store off the table kept in rdx). Decoder: honor REX.B
   in modrm base naming ([r9+d] chains rendered as rcx before - reading
   the disassembly caught it) + C6 /0 byte stores. MEASURED:
   11_closure_counter **0.971x**, 63_closures 0.985x; controls 1.0000.
   The residual jit_put_int/float/bool releases stay (a reference dst's
   release genuinely needs C++ - nothing left to inline there).
7. **The exception family** - **DONE (2026-07-25) as the INLINE forms**
   per the recorded design below (the first, helper-call attempt is kept
   for the record). PushHandler = inline capacity-check + `mov dword
   [finish], remap[target]; add finish, 4` (cold grow ->
   jit_push_handler_grow); PopHandler = `finish -= 4` (a trivial 4-byte
   VmHandler - pop_back IS the decrement); SetPend / EndFinally = a byte
   store / compare on records[rec_n-1].pend via the probed activation
   layout (jit_addr_vm_act + handlers/records/rec_n offsets +
   sizeof(VmCallRec) + ::pend; vector internals +0/+8/+16). EndFinally's
   NORMAL path falls through inline, RERAISE bails. The COLD catch-region
   ops (CatchTest/Reraise/Throw) are eligible as UNCONDITIONAL EXITS (the
   ThrowRuntimeV pattern - they are only ever entered via vm_raise's
   dispatch, already interpreted): their eligibility is what mattered,
   they were the run SPLITTERS leaving a try loop's back edge crossing
   fragments. MEASURED vs the clean base: 71_exc_no_throw **0.326x**,
   72_exc_finally **0.287x**, 69_exc_crossframe 0.985; **the
   throw-EVERY-iteration shapes REGRESS (70_exc_runtime_error +9.6%,
   42_exceptions +2.7%)** - after each throw the interpreter resumes at
   an INTERIOR pc of the merged run and cannot re-enter the fragment
   until the loop head (the interior-entry limitation; the eventual fix
   is per-pc fragment ENTRY POINTS). The trade: exceptional control flow
   as the NORMAL path pays ~3-10%, the no-throw/finally paths gain
   3-3.5x. Decoder: cmp r64,r/m64 (3B) + imul r,r/m,imm32 (69).
   (The historical helper-call attempt record:) **ATTEMPTED 2026-07-25, REVERTED
   with a measured verdict + the fix design.** The helper-call form of
   PushHandler/PopHandler/SetPend (jit_push_handler / jit_pop_handler /
   jit_set_pend over g_vm_act; the pushed catch_pc = remap[target],
   routed via emit_branch for remap access) was implemented, green
   everywhere, execution-proven - and MEASURED 71_exc_no_throw **+8.0%**,
   42_exceptions +2.7%: the per-op call protocol (~12+ insns x2 per try
   for prologue/pushes/re-mats) costs MORE than the two dispatches it
   replaces for a 4-byte vector push/pop. Slower = unfinished -> reverted.
   **The fix design (do this instead):** INLINE the push/pop fast paths -
   probe &g_vm_act + VmActivation::handlers + libstdc++ vector internals
   (_M_start +0 / _M_finish +8 / _M_end_of_storage +16, the layout the
   flat-array reads already rely on): push = cmp finish vs end_of_storage
   -> grow-HELPER on full, else `mov dword [finish], catch_pc; add
   finish, 4`; pop = `sub finish, 4` (2 insns - never empty at a
   PopHandler by codegen construction); SetPend = a byte store into
   records[rec_n-1].pend (probe VmCallRec::pend + records data + rec_n).
   THEN the raise-side ops (Throw/CatchTest/Reraise/EndFinally) need the
   DYNAMIC-RESUME design: a helper running vm_raise and returning the
   resume pc (or the jit_ret sentinels - IN-VM switches chunk via the
   resume globals, BOUNDARY returns), with the fragment doing
   `flush_cache; mov eax, <returned>; ret`; the helper needs the RUNNING
   chunk - bake the DESCRIPTOR (desc->vm_chunk, the jit_exec_block
   pattern; main has no descriptor -> main's throws stay interpreted or
   get a chunk-registry answer first).
8. **Calls - M5.** Increment 1 **DONE (2026-07-25)**: the SYNCHRONOUS
   native call - a fragment's plain CallV runs its callee TO COMPLETION
   inside jit_call_sync (the vm_try_invoke boundary machinery builtin
   callbacks already use; the callee's own fragments run within), so the
   CALLER stays native across the call. Any callee with a chunk, MAIN
   callers included; the #55 direct fragment call remains for native_leaf
   callees. Three conveyance pieces built: (1) a BAKED call-site loc
   (packed line<<32|col; ⚠ the emit must look the loc up by the OLD pc -
   the side tables are remapped AFTER emission, and a remapped lookup
   silently baked 0 -> `at line 0` in a backtrace) patched onto the
   innermost frame vm_try_invoke captures loc-less; (2) a
   **std::exception_ptr conveyance (g_vm_jit_eptr)** for PLAIN exceptions
   (a callee's UndefinedVariableEx has no clone()) - EnterNative rethrows,
   propagating exactly like the interpreted call's own throw - this
   unlocks the whole non-Runtime class no clone-based conveyance could;
   (3) a SYNC DEPTH CAP (200) bailing the deep tail to the interpreted
   CallV's flat in-VM stack (C-stack bounded). ⚠ DIRECT SELF-RECURSION is
   compile-time INELIGIBLE (jc->slot_desc[callee] == caller_desc): past
   the cap every level paid a fragment-enter -> sync-bail -> exit ->
   re-dispatch round-trip (a measured +14% on 10_recursion_deep, now
   1.004); a self-recursive caller gains nothing from staying native
   across its own call. MEASURED: neutral on the current benches - the
   hot plain-CallV shapes were already leaf-covered (08), self-recursive
   (10), or cold (43's setup call); the container-plan test's main island
   migrated CallV -> CheckFuncV/MapFilterV (the arc again). **Increment 2
   (CachedCallV + CallValueV via the same sync core) was IMPLEMENTED,
   MEASURED, and REVERTED (2026-07-25)**: 76_funcval_dispatch **+16%**,
   11_closure_counter **+17%**, fib +3.2% - the vm_try_invoke boundary
   machinery (window push + RAII + captures switch + rebind + the arg
   copy; a stack arg buffer recovered only a little) is HEAVIER per call
   than the lean in-VM `vm_enter_call` (record REUSE - the N6 win) it
   displaces, and in call-dominated loops the caller-stays-native saving
   cannot cover it. Slower = wrong-or-unfinished -> reverted same-turn.
   **THE LEAN SYNC ENTER - DONE (M5 increment 3, 2026-07-25).** Built as
   designed but with a SENTINEL instead of the run-until-pop loop (zero
   dispatch-loop changes - no per-op depth check): jit_call_sync[_cached/
   _value] push the callee via `vm_frame_setup` (non-boundary record,
   record REUSE, fast_bind straight from the caller's slot run - no arg
   copy) with **ret_chunk = the SENTINEL STOP CHUNK** (`vm_sync_stop_
   chunk()`: ExitBlock at pc 0 AND 1; rec.ret_pc = 1 by the ret_pc+1
   convention, pc 0 exists only for the walk's ret_pc-1 view), so the
   callee's ORDINARY return pop (`vm_frame_leave` - which also does a
   CachedCallV's cache store for free) resumes at the sentinel's
   ExitBlock and vm_dispatch RETURNS to the helper. A new
   `VmCallRec::sync_stop` flag (reset per push - record reuse) makes
   `vm_unwind_walk` stop there like a boundary but WITH a pop (capture
   the callee frame loc-less - the helper stamps the baked site - +
   restore captures + pop + g_vm_exc_pending + false), covering BOTH
   raise paths (vm_raise and the C++ landing pad) with one check.
   NON-OBVIOUS DECISIONS (for review):
   - Arity/bind/StackOverflow throws from vm_frame_setup -> BAIL (ret 1),
     not conveyance: they are pre-side-effect and idempotent (setup pops
     its half-frame), so the interpreted re-run re-throws byte-
     identically - much less machinery than site-stamped conveyance.
   - CachedCallV ALLOWS direct self-recursion (unlike CallV's gate):
     tree recursion is cache-bounded (the per-frame PureCache dedups the
     frontier - fib's effective depth is ~n), and gating it would
     exclude the flagship fib shape entirely; the depth cap (shared, 200)
     nets pathology. CallValueV also eligible (value self-calls are rare;
     cap nets them). The cache probe was factored (`vm_cache_probe_vals`)
     and the miss key rides rec.cache_key - stored by the normal pop.
   - **DIRECT FRAGMENT ENTRY** (the load-bearing second half): the first
     dispatch-path measurement REGRESSED tiny callees (11_closure_counter
     +6.0%, 76 +0.9% - the per-call vm_dispatch entry/exit + sentinel
     round-trip ~60 Ir dwarfs a 3-op body; profiled, not guessed). Fix:
     when the callee chunk STARTS with EnterNative (the common whole-
     native-body shape; subsumes native_leaf), the helper jit_enter's the
     fragment DIRECTLY and replicates EnterNative's post-exit faithfully:
     JIT_RET_SENTINEL -> done (frame popped, dst written, NO dispatch at
     all); a conveyed raise -> vm_raise (caret from the callee loc table,
     same-frame handler first, else the walk stops at sync_stop); a
     dispatched handler or a plain bail -> ONE `vm_dispatch(start_pc)`
     continuation (what the dispatch path always paid). ⚠ the depth
     counter MUST wrap the direct entry too - a nested direct chain
     (fib) grows the C stack with no dispatch frame, so skipping the
     counter would unbound it.
   - Plain (non-Runtime) exceptions still C++-propagate out of
     vm_dispatch -> caught -> g_vm_jit_eptr; the records above die with
     the invocation (the interpreted plain-throw contract, mirrored).
   MEASURED (callgrind Ir vs the pre-inc-1 baseline 2064987, plain
   releases): 76_funcval_dispatch **0.959**, 09_fib_recursive **0.970**,
   11_closure_counter **0.983**, 12/10/08 neutral (12's map/filter is
   VmInvoker's loop, untouched; 10 stays self-gated; 08 was leaf-
   covered). vs inc-2's reverted boundary form: 76 +16% -> -4.1%, 11
   +17% -> -1.7%. Full battery green (3x -rt, bench+samples differential,
   fuzzer 300, backtrace through nested sync calls byte-identical).
   Still open: indirect mutual recursion past the cap pays the bail
   round-trip (rare; per-pc entry points would absorb it).

   **Follow-ups landed the same day (2026-07-25):**
   - **CheckFuncV + MapFilterV nativized** (1776e72): jit_check_func (a
     non-func conveys a loc-less TypeErrorEx; the re-raise stamps arg0's
     caret at the op pc) + jit_map_filter (the shared vm_map_filter;
     MapFilterV is a pick_cached_slots BARRIER - callback writes). Bench-
     neutral (the body dominates) but it removes the LAST simple boxed
     island pair: bench/ is now **97 READY / 3 NOT-ready chunks** (the two
     compile-time-gated self-recursive CallVs + main's map pair before this
     - now just the self-gated pair). The container tests' island source
     migrated to the ONLY still-boxed sequential ops - the dyn-callee pair
     (CheckCallableV + CallValueGenericV, now in op_is_simple_island),
     keeping jit_exec_block exercised on real dyn-dispatch code.
     CORRECTION (2026-07-25, maintainer question): this pair is fully
     AST-FREE like every op (F1 step 2, 2026-07-14 - it reads the
     serializable call_sites pool; the earlier "AST-holding" wording here
     and in the code comments was WRONG and has been fixed). The VM's
     zero-AST guarantee (vm_ast_teardown + live_nodes == 0, machine-proven
     per run) is intact and the JIT added no AST dependency - fragments
     bake only pool-buffer addresses and program-lifetime pointers and run
     AFTER the teardown. Nativizing this pair is an ORDINARY emit-case
     step, not gated on any descriptor work.
   - **FloatBin mod arm** (3a1137a): the div arm's sign-stripped +-0.0
     bits test (JR_DIV0) + the exact fmod libm call TypeFloat::mod makes
     (emit_libm_call - the bracket saves rdi + pinned regs). The last
     FloatBin arm; FloatBin is now fully eligible.
   - **Suite geomean checkpoint** (one measure run, cached CPython):
     **7.84x vs CPython** (0.128x my/py over 76 paired benches) - from
     ~5.5x at the start of the model-flip arc. The remaining worst ratios
     (67_make_dict 0.73, 66_dyn_foreach 0.58, 76_funcval 0.53,
     63_closures 0.52) are VALUE-MODEL costs (dict/closure allocation,
     callback-loop protocol, boxed-value churn - the paused #60/N7 arc),
     not dispatch - the nativize-ops lane is reaching diminishing geomean
     returns; its remaining value is STRUCTURAL (deletability/.myv, the
     per-pc entry points, full-native call graph).
Plus the standing structural item: per-op loc conveyance so the side-table
re-raise ops (SubscriptV/DictLoad/boxed-arith/...) become DELETABLE (their
carets currently collide on the EnterNative pc when a run is deleted).
**Design validated (2026-07-25, not yet built):** deletability for a
throwing op = "conveys WITH ITS OWN LOC + never re-executes + never
BAILS". Key insight on why that suffices: a conveyed throw is handled
INSIDE the EnterNative handler (jit_enter returns, the g_vm_jit_exc check
runs right there - control never re-dispatches to the exit pc), so the
collapsed pc only matters for vm_raise's loc stamp - which own-loc
conveyance makes moot. A BAIL, by contrast, resumes AT the exit pc, which
in a deleted run is the EnterNative pc itself -> the fragment would
RE-RUN from the head (double execution) - hence bailing ops stay
non-deletable no matter what. Mechanism: bake `&chunk.locs[i]` (the
LocEntry - pool-buffer baking, established sound) as an extra helper arg;
the helper stamps e.loc_start/end from it before g_vm_jit_exc. Guard:
delete-originals must additionally EXCLUDE runs with ANY inline_ctxs
entries (multiple deleted ops' inlined-at chains would collapse onto one
pc - ambiguous flush). Families to convert: SubscriptV, DictLoadInt/
Float, BinOpV/CmpV/CompoundV/UnaryV, CoerceNumV, MemberV, SliceV,
LoadGlobalV (undefined-name throw), the IncDec family, the store family.

**INCREMENT 1 LANDED (ef536b7, 2026-07-25)** - the read/arith family:
BinOpV/CmpV/LogV/UnaryV/CompoundV (the BoxedOp pool gains start/end,
stamped in the helpers' catch - the pool ptr is hot-live, zero tax),
SubscriptV/SliceV/DictLoad*/CoerceNumV (loc-less helpers + the COLD-side
`g_vm_jit_lep` handoff: the emit stores the baked &chunk.locs[i] on the
FAILURE branch only, vm_raise consumes it stamp-if-empty + clear),
MemberV (already pool-stamped - free), LoadGlobalV (bail -> eptr
conveyance of the exact UndefinedVariableEx; dst|gslot packed into one
arg so the lep arg is emit-neutral). MEASURED DEAD ENDS (all rejected):
a helper ARG for the lep = +4 Ir/call (live across the try -> an extra
callee-saved spill; 62_dict_word_count +0.6%); a lep|flag TAG unpack =
+2 Ir/call; a dict-load type PRE-CHECK instead of the try = +2.0% on
25_dict_member (the try is the zero-cost-EH shape). DictLoad split into
_int/_float entry points (kills the old per-site is_int movabs):
25_dict_member **0.984x**, everything else neutral, deletions verified
(a boxed `s = s + a[i]` loop -> a bare enter.nat, caret err-loc-pinned).
bench/ surviving convey-family ops 122 -> 92; the rest sit in runs with
non-fully-native neighbors (the store family, JumpUnlessTrueV, calls).

**THE SPLIT PREDICATE (a real bug found + fixed):** native_leaf now
requires **op_never_exits** (exit-FREE), not op_fully_native: the #55
direct call IGNORES the callee fragment's return value, so a conveying
throw inside a leaf was DROPPED with the callee frame left pushed - a
PRE-EXISTING hang (CallBuiltinV was already on the old combined list;
`-ni` + a non-inlined `len(dyn)` callee printed a stale dst and hung).
Pinned by jit_leaf_never_exits. The delete gate also excludes
raise-capable runs carrying inline_ctxs entries (collapsed pcs would
merge distinct inlined-at chains); an all-never-exits run keeps its
inline entries deletable (no raise can flush them). Also fixed: a
latent noexcept std::terminate in jit_dict_load's present-key write
(a dyn-laundered wrong-typed value hit get<>'s throw outside the try).

**INCREMENT 2 LANDED (8596ef0, 2026-07-25)** - the store family:
StoreElemInt/DictStore/StoreElem2V (convey-only, cold-side caret);
StoreElemValue/StoreMemberV/chains deletable for a LOCAL/CAPTURE base
only (a GLOBAL base bails on undefined); StoreMemberV needs no stamp
(helper already pool-stamps); compound StoreGlobalV/StoreCaptureV via
the BoxedOp pool locs + the global's undefined bail -> eptr
UndefinedVariableEx. StoreElemFloat EXCLUDED (its emitted value load can
structurally bail). bench/ surviving convey-family ops 222 -> 161;
store-bench Ir neutral (deletion is structural), 25 0.984 / 09 0.970
held.

**THE SIDE-GLOBAL REMOVED (848e06e)** - increment 2 first shipped the
caret handoff through a second global (`g_vm_jit_lep`, consumed by
vm_raise), which created a NEW hazard class: an op whose one failure
branch serves BOTH a bail (no exception) and a convey could leave the
global set with no consumer, mislabeling a later unrelated loc-less
raise - patched initially by gating the emit per compile-time kind plus
a 'never store on a bail path' convention. The maintainer asked for a
real fix: `emit_exc_stamp` now writes the baked start/end Locs DIRECTLY
into the exception object in g_vm_jit_exc, guarded IN THE EMITTED CODE
by a null check (bail/eptr -> nothing written) and a loc-already-set
check. No side state exists at all, the per-site gates and the
convention are gone, and vm_raise lost its consumption block. The
emitter bakes jit_addr_exc() (the unique_ptr's storage,
static_asserted one raw pointer) + the probed Exception loc offsets;
-vdj shows the stamp qwords decoding to the exact pinned caret.

**INCREMENT 3 LANDED (3f32b72 + 4218ae9, 2026-07-25)** - the dyn-callee
generic call pair NATIVIZED (jit_check_callable: convey-only guard,
exc-stamped, deletable; jit_call_value_generic: the interpreter's exact
by-Kind dispatch over the baked CallSite pool - FuncObject via the lean
sync core after filling run[0], Builtin by Kind with the func_lv arg0
LValue* re-derive, struct via construct_struct_v; loc-less throws
self-stamped with the pool's args caret; bails: depth cap / chunkless /
undefined-global arg0 base). THE ISLAND-SOURCE HOP CHAIN IS EXHAUSTED:
no sequential op remains that op_is_simple_island admits but
op_run_eligible rejects - jit_try_container forms no containers on real
code (the two container tests retired their g_jit_container_calls
assertions; vm_exec_block stays covered by the synthetic selftest; the
machinery kept for future un-nativizable ops). And JumpUnlessTrueV -
ALREADY fully nativized (inline int/bool + jit_is_true; the 5-task list
wrongly called it un-nativized) - became DELETABLE via the cold-side
exc-stamp, so `while (dyn_flag)` bodies delete to a bare enter.nat
(caret err-loc pinned, multi-line span).

**INCREMENT 4 LANDED (the IncDec family, 2026-07-25):** all four checked
inc-dec ops deletable for non-global kinds (elem/member/chain were
already pool-loc'd - pure gating; the scalar's loc-less TypeError gets
the cold-side exc-stamp, safe on the shared bail branch because the
848e06e stamp is null-checked - no per-site gating needed, the first
increment to benefit from the structural fix). A dyn `d++` loop body
deletes to a bare enter.nat, caret pinned.

**INCREMENT 5 LANDED (2026-07-25):** LoadElemValue (OOB conveys
exc-stamped; the unreachable non-array tail conveys the interpreted
InternalErrorEx via eptr - no bail left; a 2-D read loop deletes, caret
pinned) + StoreElemFloat (emit_float_load gained a no_bail 2-way form -
float -> movsd, else -> cvtsi2sd, byte-faithful to read_float_slot's
int/bool promotion since a bool payload is 0/1 zero-extended).

**INCREMENT 6 LANDED (52af95b, 2026-07-25):** the raise-kind INT arms.
IntBin div/mod + the reg-count shifts convey via a cold
jit_raise_kind_exc (builds the exact EnterNative-constructed exception
loc-less into g_vm_jit_exc) + the exc-stamp - every IntBin arm and the
RR shifts are deletable now, kept OUT of op_never_exits (they exit by
conveying -> never leaf-safe). GOTCHA: the convey sequence is ~100 bytes
-> raise_convey_unless needs a NEAR jcc (patch8 asserted on the short
form). A div/mod/shift loop deletes to a bare enter.nat, zero-divisor
caret pinned. Int benches neutral (cold-only). The 44_primes_sqrt -9.7%
in the spot-check is the LEAN SYNC win (ff00437) surfacing on a bench
outside that increment's list - profiled + attributed.

**INCREMENT 7 LANDED (162906f, 2026-07-25): the float-arith no_bail
tier.** Every float operand read -> the 2-way no_bail form (FloatBin +
RR/RI, CmpFloatV, JumpUnlessFloatCmp, MathFnV incl. libm selectors);
FloatBin div/mod raise -> convey (the int-arm pattern). The non-throwing
float ops join op_never_exits (LEAF-safe: all-float bodies can be
native_leafs + direct-called); FloatBin div/mod op_fully_native only. An
all-float sqrt/pow/fmod loop deletes to a bare enter.nat, float div0
caret pinned. BONUS: the 2-way load shortens the hot INT-PROMOTE path
(one type compare, no jump) - 05_mixed_arith -5.9% Ir, 55_float_sum
-1.5%, 40 -0.6%. ⚠ a vacuous-smoke trap caught mid-verify: the float
smoke initially FAILED TO COMPILE (var q = a/z -> DynRequiredEx) and
"all three engines agree" was agreement on the ERROR - the -vd deletion
probe exposed it; always confirm the probe shows real chunks.

**INCREMENT 8 LANDED (2a1233f, 2026-07-26): ThrowRuntimeV.**
jit_throw_runtime builds the exact pooled exception with its pooled
caret - Runtime kinds (NotLValue/InvalidNumberOfArgs) via g_vm_jit_exc,
plain kinds (UndefinedVariable/CannotRebind*) via g_vm_jit_eptr (the M5
channel postdating the op's old re-run-to-throw exit form). Deletable;
never leaf-safe. THE RECORDED DELETABILITY LIST IS CLOSED.

**Coverage after the 8 increments (bench/, JIT-off vs on):** TOTAL
runtime ops 2886 -> 1862 (-35% of all interpreted originals deleted);
the convey-family survivors sit in runs blocked by their remaining
non-deletable neighbors - chiefly the call ops (CallV/CachedCallV/
CallValueV sync forms bail at the depth cap / chunkless callee, so they
can never delete under the current contract), the iterator/foreach
helper pairs, and the boxed-branch/handler mix in try-heavy bodies. The
next structural steps toward whole-body deletion everywhere are the M6
question (can a sync call op's bail be eliminated so calls delete?) and
per-pc fragment entry points.

(Historical note - the older tally below predates the iterator ops.)
CallBuiltinV (~294) is DONE (see
above + the deferred backlog); Halt (83) is DONE. The simple scalar islands are
EXHAUSTED - SliceV / LoadGlobalV / the container stores / the LVALUE builtins
(CallBuiltinLV) are all DONE too. GOTCHA: an op needing a CHUNK pool
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

## Deferred nativization backlog (toward "almost everything native")

The running TODO of things that would shrink islands / extend native coverage,
to do LATER, separately (don't forget these):

- **CallBuiltinV** (~294, the big island source) — DONE (2026-07-22, c606b61 +
  the exception change fa90955); see `plans/callbuiltinv-nativization.md` (#1
  exception model change, #2 op_fully_native, #3 root-caused-orthogonal).
- **`IntSubIR` (imm − reg) shape in `specialize_arith_ops`** — WON'T DO
  (2026-07-22, maintainer decision). The JIT motivation is SUBSUMED by the
  generic-IntBin task above: `0 - i` / `k - i` / `50 - s` now fragment + delete
  native via the generic path. Since subtraction is non-commutative with no
  imm-reg shape, `imm - reg` was the ONLY non-throwing generic-IntBin shape, so
  the generic emit covers exactly what IntSubIR targeted. That leaves IntSubIR a
  purely INTERPRETER-side win (a fixed op skips the 11-way `aop` switch for
  `imm - reg`) — a marginal one shape of 23, not worth a new opcode + the
  documented dispatch-layout regression risk. Skipped; the generic path is the
  fix.
- **Make a NON-div/mod/non-shift generic `IntBin` JIT-eligible** — DONE
  (2026-07-22). `jit_op_eligible(IntBin)` now gates on `aop`
  (plus/minus/times/band/bor/bxor → true; div/mod/shl/shr/ushr → false, they
  throw / need the bail), the `emit_op` case reads `in.aop` and `op_rr` emits the
  matching x86-64 instruction (load_operand on BOTH operands — unlike RR/RI which
  read a as a slot — since a generic IntBin's `a` may be the imm, e.g. `0 - i`),
  and `IntBin` is `op_fully_native` (only the non-throwing arms reach a run via
  the gate). So `abs(0 - i)` and any non-throwing generic-IntBin corner now
  fragments + deletes native. Verified: the `neg` case in `jit_delete_originals`
  (`0 - i` / `50 - s` loop deletes its int ops JIT-on) + a manual `-vd` (f's whole
  body collapses to `enter.nat`, the `i.bin` original gone). This SUBSUMES the
  `IntSubIR` shape's JIT benefit (the interpreter still uses the generic switch
  for `imm - reg` — IntSubIR would specialize that, an interpreter-only win).
- **The re-raise-op DELETABILITY refinement** (see callbuiltinv #2 "bigger
  opportunity") — make SubscriptV / DictLoad / boxed-arith / CoerceNumV
  op_fully_native (they re-raise, never re-execute) so loops with subscripts /
  dict-reads / boxed-arith delete their originals. Blocked on the loc side table
  collapsing multiple throwing ops' carets onto the EnterNative pc when a run is
  deleted; needs per-op independent loc conveyance (or a one-side-table-throwing-
  op-per-run cap).
- **Builtin FIXED arities** (see callbuiltinv #1 follow-up) — split
  `write(str[, file])` into `write(str)` + `fwrite(file_or_handle, str)` (file
  first), so the inferencer can arity-check builtin calls at COMPILE time and the
  runtime `InvalidNumberOfArgsEx` throw disappears (it could then go back to a
  hard `DECL_SIMPLE_EX`). A language/interface change — propose to the maintainer.
- **AppendV** — DONE (2026-07-23). `append(a, x)`/`push(a, x)` via jit_append -
  forms arg0's LValue* from kind (0 loc/1 gbl/2 cap) + arg0_slot, runs the
  never-throwing arr_append_fast; a decline falls back to vm_call_builtin_lv_rest
  (builtin_append), which was REFACTORED to take the BuiltinCall pool ENTRY (not
  chunk+idx, so the JIT bakes `&builtin_calls[idx]`; the interpreter's AppendV/
  CallBuiltinLV pass chunk->builtin_calls[idx]). Every reachable append throw is a
  RuntimeException now (NotLValueEx / TypeErrorEx / **CannotChangeConstEx made a
  RuntimeException** - the maintainer-pre-approved lvalue-builtin exception fix,
  now inherently-dynamic + permanent like InvalidArgumentEx; script-CATCHABLE,
  README + a test updated). An undefined-global arg0 -> null target ->
  NotLValueEx (no bail needed). NOT op_fully_native. VERIFIED: append loop native
  + correct, flat-mismatch TypeError byte-identical, const-append now catchable.
- **CallBuiltinLV** (pop/insert/erase/sort/reverse/intptr) — DONE (2026-07-24)
  via `jit_call_builtin_lv`. Forms arg0's LValue* from kind (0 local/1 global/2
  capture) + arg0_slot; `rest_base >= 0` -> a rest-native op
  (vm_call_builtin_lv_rest with the value-arg run), `rest_base == -1` -> a
  no-value-arg op (func_lv with an empty rest). The ArgLocs it builds inline
  matches Chunk::arglocs_at; `bc` = the baked &builtin_calls[idx] buffer ptr.
  Every reachable throw is a RuntimeException now (CannotChangeConstEx was made
  one for AppendV) -> g_vm_jit_exc + re-raise (loc from the pool if loc-less).
  An undefined-global arg0 -> null target -> NotLValueEx (no bail). NOT
  op_fully_native, NOT cached (rest-run layout unknown at classify time). Encodes
  has_rest into rest_base (-1 sentinel) to fit 5 register args. VERIFIED: -vdj
  fragment covers the call.blt.lv ops with a native call; jit_op_nativized
  CallBuiltinLV case (insert+pop loop) proves the counter bumps; insert-OOB /
  const-mutation (catchable) / undefined-global-target byte-identical vm/nj/tw;
  discarded `pop(a);` writes a temp (interpreter doesn't discard LV dst) - OK
  under hardening.
- **CallBuiltinLVElem / CallBuiltinLVMember** (the subscript/member arg0 variants:
  `append(a[i], x)`, `append(s.f, x)`) — DONE (2026-07-24) via
  `jit_call_builtin_lv_elem` / `_member`. Same 5-arg shape as CallBuiltinLV
  (kind, base_slot, dst_slot, run_base, &builtin_calls[idx]) but `b` is always a
  lit (the value-args run - no -1 sentinel needed). Elem: form the base's LValue*
  by kind, then the element's via the runtime Type::subscript (the SAME COW path
  Subscript::do_eval uses; a non-lvalue element -> null target -> NotLValueEx),
  run[0] = the index + run[1..] = the values (a `holder` EvalValue keeps the
  subscript result alive across func_lv). Member: form the boxed field LValue*
  via vm_member_lvalue (needs `m->base_struct` - a PROVEN struct base; a dyn/dict
  base takes the MemberV+AppendV path instead, so the op only fires for a typed
  struct), run[0..] = the values (NO index). Both stamp arg0's caret
  (`bc.args[0]`, the a[i]/s.f target) if loc-less; every throw is a
  RuntimeException. NOT op_fully_native, NOT cached. VERIFIED: -vdj fragment
  covers both call.blt.lve/lvm ops with native calls; jit_op_nativized cases
  (dyn `append(rows[k],i)` loop; `Bag b` `append(b.items,i)` loop) prove the
  counters bump; OOB-subscript / flat-scalar-NotLValue / const-struct-field
  (read-only -> NotLValue) errors byte-identical vm/tw.
- **MakeArrayV** (the first CONTAINER BUILD) — DONE (2026-07-24) via
  `jit_make_array` (rdi=dst, rsi=run base, rdx=n, rcx=the ArrHint from
  `target2`). It calls the file-local `vm_make_array`, i.e. the interpreter's
  exact path into the shared `build_array_from_values` - so every storage kind
  (flat int/float/bool, flat POD-struct, general, and the empty-with-hint forms)
  builds byte-identically. **It NEVER THROWS** (the build has no error path - a
  mixed literal just goes general), so the helper returns void and the op is
  **op_fully_native**: a loop that builds a per-iteration array literal now
  DELETES its interpreted originals down to ONE `enter.nat` and the body becomes
  a **`native_leaf`** (call-able by a caller fragment, #55) - the first container
  build to reach that state. The element buffer stays in `vm_make_array`'s frame
  (NEVER inline it - vm_run_chunk's frame is multiplied by VM recursion depth;
  see that helper's comment). N5: the element run IS enumerable from the
  instruction (base + n), so `pick_cached_slots` disqualifies `[a_lit,
  a_lit+b_lit)` + dst precisely rather than turning caching off for the run - an
  element can be a cached int counter (`[i, i*2]`). **The jit_container test's
  island moved MakeArrayV -> MakeDictV** (the next still-boxed simple op; the
  same hop SliceV -> MakeArrayV made). Verified perf-safe by the SliceV/MakeArrayV
  precedent: a temporary `g_jit_container_calls` probe over ALL of bench/ and
  samples/ found **ZERO** containers formed (and the probe was itself validated -
  it reported 6 island calls on the known dict-literal container shape, per the
  "prove the code ran" rule). VERIFIED: -vdj fragment covers the make.arr op with
  a native call; a jit_op_nativized MakeArrayV case proves the counter bumps; all
  storage kinds + the >16-element heap path + empty/struct-hint literals agree
  vm/nj/tw; bench + samples vm-vs-tw output identical (the two "diffs" are
  rand_sort's rand() and shopping's timeout-truncated interactive loop).
- **MakeDictV** — DONE (2026-07-24) via `jit_make_dict` (rdi=dst, rsi=run base,
  rdx=npairs) -> the file-local `vm_make_dict` -> the shared
  `build_dict_from_pairs` over the INTERLEAVED key/value run, so the VM, the JIT
  and the tree-walker share one builder (incl. the `make_const_clone` key
  FREEZE). Pair buffer stays in vm_make_dict's frame (the recursion-depth rule).
  **Unlike MakeArrayV it CAN THROW:** freezing/inserting a key HASHES it, and the
  base `Type::hash` throws `TypeErrorEx` for an unhashable value - reachable with
  a FUNCTION key laundered through `dyn` (`var dyn d = {k: 1}`). So the helper
  returns a status, re-raises via g_vm_jit_exc, and the op is **NOT
  op_fully_native** (its caret comes from the pc-keyed loc side table).
  **⛔ PRE-EXISTING BUG FOUND + FIXED:** the interpreter's MakeDictV handler was
  commented "never throws (all values hashable)" and had NO loc recorded, so that
  TypeErrorEx surfaced under `-vm` with **no caret at all** ("at line 0", no
  source line) while the tree-walker pointed at the `{...}` literal. Fixed in the
  same change: the codegen records the LiteralDict node, `extract_locs` puts its
  span in the loc side table, and the handler stamps it via `vm_stamp_loc` - the
  two engines are now byte-identical (pinned by an `err loc:` test). This is the
  general lesson for a "never throws" comment: VERIFY it against the type
  system's error paths before trusting it (a container build that HASHES can
  throw; one that only stores cannot - MakeArrayV genuinely never throws).
  N5: the interleaved run [a_lit, a_lit + 2*b_lit) + dst are disqualified (a key
  or value can be a cached int counter - `{i: i * 2}`). **The jit_container
  test's island moved MakeDictV -> StructCtorBoxedV** (a NON-POD struct
  construction with runtime args - the rarest remaining boxed build); perf-safety
  re-checked with the `g_jit_container_calls` probe over ALL bench/ + samples/ =
  ZERO containers formed, the probe itself validated (6 island calls on the known
  shape). VERIFIED: -vdj covers the make.dict op with a native call; a
  jit_op_nativized MakeDictV case proves the counter bumps; the unhashable-key
  throw is byte-identical vm/nj/tw AND catchable from inside a fragment;
  multi-pair / nested / mixed-key / >8-pair-heap literals all agree.
- **The STRUCT BUILDS (StructCtorV / StructCtorBoxedV / MakeStructArrayV)** —
  DONE (2026-07-24). Three helpers, each the interpreter's exact call:
  `jit_struct_ctor(def, base, nf, dst)` (POD `P(x,y)` -> `vm_struct_ctor`, incl.
  the **H1 DST-SLOT REUSE** - VERIFIED still firing under the JIT: an
  intptr-identity loop counts 999/999 reuses JIT-on vs JIT-off),
  `jit_struct_ctor_boxed(dst, base, &boxed_ctors[idx])` (non-POD `B(a,x)` ->
  `vm_struct_ctor_boxed`; also the CHECKED POD ctor), and
  `jit_make_struct_array(def, base, n, dst)` (the fused flat `array<PodStruct>`
  literal -> `vm_make_struct_array_op`; `n` is the ELEMENT count, the run holds
  n * nfields values). The `StructTypeDef*` from the struct_defs pool is baked as
  a VALUE (program-lifetime, like MakeClosureV's descriptor); the boxed ctor
  bakes `&boxed_ctors[idx]` (pool buffer address).
  **Exceptions:** every reachable throw is a `TypeErrorEx` from
  `coerce_struct_field` / `coerce_to_decl_type` - a RuntimeException - so
  `catch (RuntimeException)` suffices and NO exception-model change was needed
  (checked explicitly: no arity/DECL-style exception can escape, the codegen and
  inferencer fix arity at compile time). The POD ctor + struct-array literal
  throw LOC-LESS (their gates make it defensive-only) and get the side-table
  caret at the EnterNative re-raise; the BOXED ctor's throw already carries the
  offending arg's POOLED per-arg caret, which survives because **`vm_raise`
  stamps only an EMPTY loc**. None is op_fully_native.
  **N5:** StructCtorV's field run IS enumerable ([a_lit, a_lit+b_lit)) so it is
  disqualified precisely (plus dst - the H1 reuse READS the current dst value);
  StructCtorBoxedV and MakeStructArrayV return `{}` (no caching for the run) -
  their run LENGTH needs the boxed_ctors pool / the def's field count, and
  pick_cached_slots has no chunk, i.e. the MakeClosureV "can't enumerate ->
  disable" rule. **The jit_container test's island moved StructCtorBoxedV ->
  DeclConstV** (a `const` array/dict decl inside a function body - a
  non-branching data op that is rare in real code); perf-safety re-checked with
  the container probe over ALL bench/ + samples/ = ZERO containers formed, probe
  self-validated (6 island calls on the known shape). VERIFIED: one loop body
  carrying all three shows `struct.ctor` / `struct.ctor.b` / `make.structarr` all
  inside ONE fragment (`-vdj`); three jit_op_nativized cases prove the counters
  bump; the boxed ctor's PER-ARG caret (pointing at the offending arg, not the
  call) is byte-identical vm/nj/tw and catchable from inside a fragment, as is
  the checked-POD path.
- **The FOREACH element/field LOADS** (LoadElemValue / LoadElemBool / StrLen /
  LoadStrChar / LoadStructFieldInt / LoadStructFieldFloat / LoadStructElemV) —
  DONE (2026-07-24). Six helpers (the struct-field pair shares one with an
  `is_float` selector), each the interpreter's exact body. **The INDEX is passed
  as a VALUE:** the emitter materializes the op's slot-or-literal `a` operand
  with the cache-aware `load_operand` into rax BEFORE `emit_call_prologue` (which
  pushes rdi + the cache regs but does not clobber rax), then movs it into the
  arg register. That is what keeps the FOREACH COUNTER - the slot N5 most wants
  to pin - a countable, cacheable int use instead of a disqualified one; the dst
  and base are disqualified (the helper reads/writes them in memory). All are
  op_fully_native EXCEPT **LoadElemValue**, which bounds-checks (it also serves
  2-D `a[i][k]` reads whose index is not the loop counter): an OOB sets
  g_vm_jit_exc LOC-LESS so EnterNative stamps the side-table caret
  (byte-identical vm/nj/tw), and a non-general/non-str base BAILS so the
  interpreter re-raises its InternalErrorEx identically.
  **⛔ THREE SHAPE TRAPS these ops teach (all found by the per-op counter, none
  visible to the differential):** since they are op_fully_native their
  interpreted originals are DELETED, so `-vd`/`-vdj` show only the `enter.nat` -
  **the runtime counter is the ONLY evidence**, exactly the standing rule. (1) The
  struct ops need a PROVEN `array<S>` container; a `dyn` one routes to
  ForeachDynInit/Next and never reaches them (my first test shapes used `dyn` and
  the counters stayed 0). (2) `s += p.x` FUSES into StructFieldAddInt (#9), so a
  field-read test must use a NON-accumulator body. (3) A body containing a BOXED
  branch (`if (b == true)` -> JumpUnlessTrueV, still un-nativized) SPLITS the run
  at the branch, leaving the loop's back edge outside the fragment - so only the
  loop-ENTRY copy runs native and **the counter stops scaling with iterations**
  (LoadElemBool bumped 1 for a 100-element array). With a branch-free body the
  same loop bumps 50/50 and 26/26. That makes **JumpUnlessTrueV the next real
  blocker for foreach loops** - it is worth more than any remaining load.
- **JumpUnlessTrueV** (the boxed condition branch - the top blocker) — DONE
  (2026-07-24). The FIRST non-arithmetic op to join `op_is_branch` /
  `emit_branch`: `jit_is_true(cond_slot)` evaluates the condition and returns a
  **TRI-STATE** (1 true / 0 false / **-1 threw**, because `is_true` is a virtual
  Type op whose BASE throws for a value with no bool conversion - reachable with
  a builtin laundered through `dyn`), and the FRAGMENT does the jump: `test eax`
  + `jns` past an `exit_pc` for the throw, then `test eax` + a `jz` through
  `emit_cond_jump_raw` (fragment-local jcc for an in-run target, exit_pc
  otherwise). N5: the condition slot is disqualified (read from memory; it holds
  a boxed value anyway). NOT op_fully_native (side-table caret).
  **WHY IT WAS THE TOP BLOCKER, measured:** an un-nativized branch SPLITS the
  run, so a loop body containing one left the back edge OUTSIDE the fragment and
  every iteration after the first ran interpreted. The same bool-foreach loop
  that bumped `LoadElemBool` **1 time for a 100-element array** now bumps **100**
  (and LoadStrChar 1 -> 25); its container plan went "NOT ready - 1 island" ->
  "READY - whole body native". So this single op unblocked the entire previous
  batch in branchy bodies - the compounding the arc predicts.
  **⛔ AND IT EXPOSED A REAL JIT CRASH IN LogV (fixed here).** `jit_boxed_log` is
  `noexcept` and called `is_true()` with NO try/catch, because LogV had been
  nativized under the comment "eager && / || (is_true), never throws" - the same
  wrong assumption that cost MakeDictV and JumpUnlessTrueV their carets, but here
  the escaping exception hit **std::terminate**: `if (n > 0 && x)` with an
  unconvertible `x` CRASHED the JIT (`-nj` merely lost the caret). Fixed: the
  helper returns a status and re-raises, the emit gained the `test eax` +
  exit_pc, LogV left `op_fully_native`, and its loc is now recorded (the codegen
  already attached the `&&` node - extract_locs was dropping it). Both VM engines
  now caret the &&-expression; the tree-walker carets the offending OPERAND, a
  residual VM-vs-tw difference of the same kind already documented for UnaryV, so
  the regression test pins the TYPE only.
  **THE STANDING LESSON, now three-for-three:** every "never throws" comment on
  an op that calls a VIRTUAL Type op (is_true / hash / to_string / a coerce) must
  be checked against that op's BASE implementation, which throws. Two cost a
  caret; one cost a crash.
- **Halt** (83, the biggest blocker) — DONE (2026-07-23). A fall-through body's
  implicit `return none` runs in the fragment via `jit_halt` - EXACTLY ReturnV's
  jit_ret with the result hard-wired to none (no slot): IN-VM frame ->
  vm_frame_leave (parent dst = none) -> JIT_RET_SENTINEL; BOUNDARY frame (main /
  a callback) -> bare JIT_RET_BOUNDARY (flow untouched - a fall-through body's
  flow is none, unlike ReturnV's boundary which sets flow=ret). op_fully_native
  (rets a sentinel, never an interior pc -> deletable), no slots in
  pick_cached_slots (like Jump). The "native_leaf needs a trailing ReturnV"
  concern was a non-issue: a Halt-ending body is simply NOT a native_leaf (not
  native-CALLABLE), but its OWN fragment still runs the Halt natively. VERIFIED
  (jit_op_nativized Halt case - a void func's whole body deletes to one
  enter.nat; both paths exercised: f's Halt boundary=0 IN-VM returns none to the
  caller, main's Halt boundary=1 ends the program).
- **LoadGlobalV** — DONE (2026-07-23) WITHOUT the language change. Its undefined-
  global throw is UndefinedVariableEx (a plain Exception, not conveyable via
  g_vm_jit_exc), but rather than making it a RuntimeException (the CallBuiltinV #1
  precedent - a semantic change: undefined-var would become catchable), it uses
  the **N4 bail-to-reinterpret** pattern: the COMMON case (defined global) runs
  native via jit_load_global; the RARE use-before-def BAILS (helper returns 1, no
  g_vm_jit_exc/raise set), and the emit's exit_pc resumes the INTERPRETER at the
  op's pc so it re-runs LoadGlobalV + throws - semantics-PRESERVING (undefined-var
  stays a non-catchable hard error) and byte-identical to a non-JIT run (VERIFIED:
  vm-bail caret == nj-interpreted caret; the vm-vs-tw caret diff is PRE-EXISTING,
  differential-invisible). NOT op_fully_native (the original is kept for the bail).
  EXECUTION-PROVEN: jit_op_nativized LoadGlobalV case (hot loop reading a defined
  global, counter bumps) + a use-before-def-global `tests` entry (the bail throws
  UndefinedVariableEx on both engines).
- **SliceV** — DONE (2026-07-23). A slice `base[start:end]` runs native via
  jit_slice (the runtime Type::slice - the COW-registered sub-view; start/end ==
  -1 -> none). Only TypeErrorEx (a non-int bound / non-sliceable base) is thrown
  (slices CLAMP out-of-range, no OOB), and it is a RuntimeException -> caught
  loc-less into g_vm_jit_exc + re-raised (byte-identical caret across
  vm/nj/tw, VERIFIED, + catchable in a fragment). NOT op_fully_native (side-table
  caret). **The jit_container test's island moved from SliceV to MakeArrayV** (an
  array literal `[a]` - now the container gate's only non-op_run_eligible
  op_is_simple_island source; ALL simple scalar ops AND SliceV are nativized).
- **StoreElemValue** — DONE (2026-07-23). The UNIVERSAL subscript store
  `a[i] = v` / `a[i] OP= v` (flat / general / dict, ANY base) via
  jit_store_elem_value -> the interpreter's exact vm_subscript_store. Unlike
  StoreElemInt/DictStore (a LOCAL frame-slot base lea'd in the emit), the base
  may be GLOBAL/CAPTURE, so the emit passes `kind` (0/1/2, from `target`) + the
  base slot and the helper forms the LValue*. TWO throw sources, both `return 1`
  (EnterNative distinguishes by whether g_vm_jit_exc is set): (1) an UNDEFINED
  GLOBAL base bails-to-reinterpret (no exc, N4-style like LoadGlobalV -> the
  interpreter re-runs + throws UndefinedVariableEx); (2) vm_subscript_store's
  OOB/KeyNotFound/TypeError/NotLValue (all RuntimeException) -> g_vm_jit_exc ->
  re-raise. NOT op_fully_native. aop is passed in r8 (5th arg; re-materialised to
  t_float by the epilogue after the call). EXECUTION-PROVEN: jit_op_nativized
  StoreElemValue case (general-array store loop) + verified all 4 paths
  (general/global native, OOB re-raise byte-identical, undefined-global bail
  vm==nj) + a use-before-def-global-store `tests` entry.
- **StoreMemberV** — DONE (2026-07-23). A struct field store `s.f = v` /
  `s.f OP= v` (a dict member store uses DictStore) via jit_store_member -> the
  interpreter's exact vm_member_store. Like jit_store_elem_value but the key is a
  MEMBER: the emit bakes `&chunk.member_keys[idx]` (pool buffer addr, r8) and the
  helper reads memUid + the 4 carets. GLOBAL/CAPTURE struct base too. vm_member_
  store throws only RuntimeExceptions (non-struct/bad-POD-type -> TypeErrorEx
  already carrying its caret; readonly -> NotLValueEx(mstart); a compound div/mod
  -> loc-less, stamped with the MEMBER caret here exactly as the interpreted
  catch does) -> re-raise; an undefined-global base bails. VERIFIED: POD +
  boxed field native, readonly-store NotLValueEx byte-identical vm/nj/tw.
- **The nested-chain stores** — DONE (2026-07-23, for completeness). All 3 run
  native, each the interpreter's exact function: **StoreElem2V** `a[i][j]=v`
  (jit_store_elem2 -> vm_nested_subscript_store; LOCAL base, no kind); **Store
  ElemChainV** `a[k0..kn]=v` (jit_store_elem_chain -> vm_chain_store_op; `kind`
  base, key RUN [kbase,+nkeys)); **StoreLValueChainV** `base.step..=v` mixed
  member/subscript (jit_store_lvalue_chain -> vm_chain_lvalue_store_op; `kind`
  base). Pool data is baked (can't bake &chunk - it dangles): StoreElem2V bakes
  chain_locs[idx].data() (r9), StoreElemChainV bakes &chain_locs[idx] (r9,
  data()/size() in the helper), StoreLValueChainV bakes &chain_steps[idx] (r8) +
  member_keys.data() (r9). REFACTOR: vm_chain_walk + vm_chain_lvalue_store_op now
  take the member_keys buffer + the chain_steps entry (not &chunk + steps_idx) so
  the JIT can bake them; the interpreter + IncDecChainV pass chunk.member_keys.
  data() / chunk.chain_steps[idx]. All throw only RuntimeExceptions (per-step
  carets stamped by the vm functions) + an undefined-global base bails (the two
  kind'd ones). 6 args each (r9 = the 6th; r9 isn't a persistent singleton, unlike
  rsi/r8). VERIFIED: all 3 native + correct, throw carets byte-identical vm/nj/tw.
- **StoreCaptureV** — DONE (2026-07-23). A PLAIN capture store `cap = v`
  (jit_store_capture - the capture-slot twin of jit_store_global:
  `(*ctx->captures)[cap_slot] = RValue(*src)`; a capture is always defined so NO
  defined check + never throws -> op_fully_native). Same emit shape as
  StoreGlobalV.
- **The COMPOUND global/capture stores** (`g OP=`/`g++`, `cap OP=`/`cap++`) —
  DONE (2026-07-23). Extend StoreGlobalV/StoreCaptureV to their compound forms
  via jit_store_global_compound / jit_store_capture_compound - like CompoundV but
  on a GLOBAL/CAPTURE slot: they REUSE the boxed_ops pool (build_boxed_ops now
  also processes an aop!=invalid StoreGlobalV/StoreCaptureV; bo->target = the
  slot, bo->a = the rhs, bo->aop) so the rhs operand + slot ride the pool (1 arg,
  &boxed_ops[target2]). An undefined-GLOBAL base BAILS (like LoadGlobalV; captures
  never bail); a num_bin_op div0/type throw re-raises (loc from the loc side
  table). **op_fully_native now takes the Instr** (not just OpCode) so it gates
  StoreGlobalV/StoreCaptureV on aop==invalid: PLAIN = deletable, COMPOUND = kept
  (the bail/re-raise needs the original). VERIFIED: `g += i`/`c += i` native +
  correct, div0 byte-identical, undefined-global compound bail vm==nj. With this
  EVERY container/slot store - plain AND compound - is native.

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

## Fresh M4b measurement (2026-07-28, post-10x) - THE PRIZE IS COLLECTED

Question: with today's op coverage, would resuming M4b (multi-island exit
dispatch, richer islands, leaner executor) pay?

Static landscape (the M1 container-plan over ALL of bench/ + samples/):
**100% of chunks are READY** - 100/100 bench chunks, 18/18 sample chunks,
every op native-ELIGIBLE. At M1-landing time real loops were multi-island
(init+body split, boxed ops, calls); the nativize-ops arc emptied it.

Dynamic residue (callgrind vm_dispatch SELF, exclusive, scale 1):
**47 instructions TOTAL** - a constant (invocation entry + Halt) - on
01_while, 09_fib, 10_recursion, 11_closures, 31_str, 34_sort, 35_map,
43_sieve, 46_matrix, 47_wordcount, 62_dict, 66_dyn_foreach. The
interpreter loop does not execute in the steady state ANYWHERE on the
suite; the runtime already IS "native driving, C++ helpers for the hard
ops" - reached from the bytecode side by growing the islands to cover
everything, instead of flipping the driver.

The one nonzero residue: 70_exc_runtime_error - vm_dispatch self 10.02%
(the post-throw handler/resume runs interpreted until the next
EnterNative) + ~28% RTTI dyncast + ~9% strcmp (catch-type matching).
An exception-PATH cost, cold in real programs; a #60-class item
(RTTI/name-matching lean-out), NOT a flip target.

CONCLUSION: M4b as a PERF item is moot - there is no dispatch left to
capture (<0.01% suite-wide). What remains of the flip is the
ARCHITECTURE milestone only: M5-flip is largely subsumed by the M5b/c
native sync calls; the live remainder is **M6** - delete-originals
universally + the `.myv` serializer (the interior entry points from the
per-pc work already relaxed the old single-entry deletion constraint).
Disposition of task #56 is the maintainer's call: recommend retiring
"M4b/M5" and re-scoping #56 to M6/.myv.

## Delete-originals increment 1 (2026-07-28) - LoadElemInt/Float

Audit tool: MYLANG_DELAUDIT=1 prints each non-deletable run + reason +
blocking ops. Corpus baseline: 58 kept runs (0 multi-entry, 2
inline-raise, 56 bail-op: LoadElem* 46, calls 38, AppendV 10,
exceptions 16, StructFieldAddInt 5, misc).

Inc 1: LoadElemInt/Float lose their bails - inline flat fast path +
a slow-tier helper (the interpreter's exact shared core; OOB conveys,
InternalErrorEx via eptr) -> op_fully_native -> originals delete.
Corpus: 58 -> 45 kept runs. Ir: 15_slice -32.2%, 18_foreach -10.1%,
14 -3.0%, 43/46 neutral. Next: AppendV (same helper pattern), then the
call ops (need an EnterNative-performs-the-call design), then
Catch/Reraise/Throw (exit-at-op natives).

## Delete-originals increment 2 (2026-07-28) - the LV-builtin family

AppendV/CallBuiltinLV/CallBuiltinLVElem/CallBuiltinLVMember classified
op_fully_native: their helpers already run the full interpreter path and
pool-stamp their carets (collapse-safe); added the catch(...) ->
g_vm_jit_eptr net (a plain Exception would std::terminate the noexcept
helper) + an emit-side exc-stamp layer + the classification. Corpus
45 -> 41 kept runs; Ir neutral. PRE-EXISTING finding (parent-verified):
a throwing sort-comparator's div0 caret diverges between engines (VM =
the divisor operand, tw = the whole chain) - a tw stamp-granularity
item, untracked. Remaining: calls 38 (needs the EnterNative-performs-
the-call design), exceptions 12, StructFieldAddInt 5, MultiUnpackV 3.

## The CALLS deletability design (#56; agent-planned 2026-07-28, steps 1-4)

Decline inventory (all `return 1` in vm.cpp; the emitted sequence itself
never bails): D1/D4/D7 = the depth cap (jit_call_sync/_cached/_value
heads); D2/D5 = undefined global callee; D3/D6/D8 = non-callable callee;
D9 = chunk-less callee (core; NOTE the helper never tries the lazy
vm_func_chunk the interpreted op does); D10 = a vm_frame_setup throw
(arity/bind-coerce/StackOverflow). Plus the #55 direct native_leaf CallV
(already conveys, loc-less) and the compile-time unarmed self-recursion
gate (not a decline; sanitized builds just delete fewer runs).

Mechanisms:
1. ERRORS (D2/3/5/6/8/10 + #55 stamp) -> CONVEY: NotCallable/arity/
   StackOverflow are RuntimeExceptions -> g_vm_jit_exc + emit-side
   exc-stamp; UndefinedVariableEx is PLAIN -> construct with the baked
   callee caret (a locs-entry pointer arg) + g_vm_jit_eptr (rethrown
   uncatchable at EnterNative - the LoadGlobalV precedent). Bind throws
   record no callee frame interpreted -> same conveyed (parity).
2. D9 -> the helper first LAZY-tries vm_func_chunk (the interpreted op's
   own net), then PERFORMS the boundary call itself (vm_call_func /
   vm_cached_call with null chunk - the interpreted op's tail verbatim),
   converting g_vm_exc_pending with the baked site (the code already in
   the core's pending block); catch(...) -> eptr net.
3. D1/D4/D7 (the cap) -> the new resume status JIT_RET_SWITCH
   ((size_t)-3): the helper does the interpreted op's own in-VM FLAT push
   (vm_frame_setup with ret_pc = the baked post-call ENTRY-STUB pc via
   entry_remap[old_pc+1]; CachedCallV probes the cache first, the miss
   key rides rec.cache_key) and returns -3; consumers: EnterNative
   (switch chunk/pc, ZERO new C frames - the flat interpreted-call
   shape), the inline call-rdx site + the core's direct branch (one
   TRANSIENT dispatch frame at the cap transition via a shared
   jit_sync_dispatch_switch), vm_invoke_postexit (branch on -3 BEFORE
   pc-interpretation; VmInvoker's sentinel ML_CHECK stays valid). C-stack
   bound unchanged (cap x per-level frames + O(1)). Backtrace caret: a
   deleted run's loc_at(ret_pc-1) collapses -> VmCallRec gains a packed
   `call_site` (baked site_packed; zeroed in vm_frame_setup*, gated
   !sync_stop) preferred by vm_capture_rec_frame.
4. Classification: op_fully_native += the three calls (NEVER
   op_never_exits); generalize the entries/dual-remap/rebuild so stubs
   emit INSIDE deleted runs; thread entry_remap into emit_sync_call_
   inline; jit_call_value_generic keeps the old bail via a core mode
   flag (CallValueGenericV migrates later).

Risks recorded: VmCallRec::call_site staleness on record reuse (zero on
push + !sync_stop gate + a reuse backtrace test); two extra stores on
the hot interpreted call path (callgrind 10_recursion_deep gate); a
missed -3 consumer (add a guard in jit_sync_postexit); parked-key
hygiene (defensive clear); pin caps before target chunks compile in
tests. Expected end state: corpus kept runs 41 -> ~5 (exceptions +
MultiUnpackV remain).

## Calls step 1 LANDED (2026-07-28) - the error declines convey

D2/D5 (undefined callee): UndefinedVariableEx constructed WITH the baked
callee caret (the new `lep` = &chunk.locs[i] sixth helper arg) -> the
g_vm_jit_eptr channel; D3/D6/D8 (non-callable): NotCallableEx ->
g_vm_jit_exc + the slow tail's new emit_exc_stamp; D10 (setup throws:
arity/bind-coerce/StackOverflow): the core's catch conveys (Runtime ->
exc, plain -> eptr) instead of declining. The #55 direct-call null
branch gained the exc-stamp too. Status protocol: 2 = conveyed (the
existing EnterNative flag dispatch); 1 = decline remains ONLY for the
depth cap (D1/D4/D7) and the chunk-less net (D9) until the SWITCH step.
Parity: undefined/not-callable/arity/value-callee errors byte-identical
vm == jit-off == tw (except a PRE-EXISTING VM-wide gap, verified at
HEAD: an uncaught PLAIN exception thrown mid-call-stack renders NO
backtrace under the VM while the tree-walker prints frames - the plain-
exception unwind never captured in-VM record frames; a #75-class
follow-up, untracked). 10_recursion_deep Ir neutral (the lep movabs is
cold-path only).

## Calls steps 2-4 LANDED (2026-07-28)

Step 2: jit_sync_boundary_call (the chunk-less callee runs the
interpreted tail in the helper; lazy vm_func_chunk first). Step 3: the
SWITCH protocol exactly as designed (JIT_RET_SWITCH=-3;
jit_call_sync_switch pushes with ret_pc = the baked post-call stub pc;
EnterNative/inline-site/core/invoke consumers; depth untouched - flat
continuation; VmCallRec::call_site_packed for collapsed-loc backtraces).
Step 4: op_fully_native += the calls; stubs materialize inside deleted
spans. Corpus 41 -> 27 kept runs; call benches Ir flat-to--0.4%. The
switch is execution-proven at cap 4 (mutual recursion depth 60 + a
throw from depth 50 below the cap - g_jit_sync_switch bumps, catch
byte-identical). Remaining blockers: the exception trio (12),
StructFieldAddInt (5), MultiUnpackV (3), LoadMember guard-miss (3),
JumpUnlessElemInt (3), MapFilterV (2), inline-raise guard (5).

## The native `throw` + the CATCH-DISPATCH redesign (deferred) - 2026-07-28

`Throw` is now fully native/deletable (jit_throw: build + vm_raise; the
three outcomes dispatched / boundary / conveyed - see CLAUDE.md). What
remains of the "exception trio" is CatchTest + Reraise, whose originals
CANNOT be deleted with the current design because the raise machinery
DISPATCHES INTO THEM BY PC: vm_dispatch_exc sets pc to the handler's
CatchTest, and a deleted run has no such pc (every pc collapses onto the
head EnterNative).

IDEAS SAVED for that redesign (maintainer deferred it 2026-07-28):
1. MOVE THE MATCHER INTO THE RAISE PATH. The catch-type list is already
   a serializable pool (catch_types + the #74 catch_uids); vm_raise could
   walk a per-chunk HANDLER TABLE ({try-region pc range -> clause list,
   bind slot, body pc}) and jump straight to the BODY, which is ordinary
   code (already entry-mapped since #74 inc 2). CatchTest/Reraise then
   disappear from the bytecode entirely rather than becoming deletable -
   the cleanest end state, and it also removes the per-throw op dispatch.
2. HANDLER ENTRY STUBS. Keep the ops but give each handler pc an
   EnterNative stub like the post-call resumes, so a deleted run still
   has a real pc to dispatch to. Cheaper to build; keeps the matcher in
   bytecode.
3. Either way `rethrow` (Reraise) follows the matcher: it is the "no
   clause matched" tail of the same chain.
Prerequisite for both: the handler-table data must be built at codegen
(it exists implicitly in PushHandler's target + the CatchTest chain).

## Delete-originals: the FINAL batch (2026-07-28) - corpus 58 -> 7

EmplaceStruct / MakeStructArrayV / UnpackElem{Int,Float,Value,Targets}
classified fully-native (exc-stamps + eptr nets; their carets already
pooled). The inline-raise guard relaxed to "DISTINCT chains" - one
chain in a run is harmless (all entries remap to the head EnterNative
with the same index; inline_frame_at finds the right one), which is the
common one-inlined-body-per-run shape.

Corpus history: 58 (audit baseline) -> 45 (LoadElem) -> 41 (LV family)
-> 27 (calls) -> 22 (small batch) -> 20 (ForeachDyn) -> 16 (#9 fusions)
-> 15 (native throw) -> **7**.

## MakeDictV + the FINAL audit (2026-07-29) - corpus 11

RE-AUDITED after samples/phonebook started compiling again (the
value-template inference fix that same day): it had contributed NOTHING
to the "7" above, because it did not compile. With it the corpus is 12,
and one of its two runs exposed a REAL gap the final batch had missed -
`MakeDictV`, the dict-literal builder. Its `MakeArrayV` twin is
never-exits ("no error path"); a dict differs only in that it FREEZES and
HASHES each key, so an UNHASHABLE key (a func) throws. It got the
ordinary convey treatment - `emit_exc_stamp` on the failure branch plus a
`catch (...)` -> eptr net in jit_make_dict - and joined op_fully_native.
Pinned: the unhashable-key error is byte-identical across engines AND
through an image. Corpus 12 -> **11**.

THE REMAINING 11, exactly:

    8  CatchTest + Reraise    42_exceptions, 69, 70, 71, primes2,
                              phonebook, shopping x2
    1  EndFinally             72_exc_finally (its cold reraise bails)
    2  distinct inline chains 09_fib_recursive

The first 9 are the DEFERRED catch-dispatch work (see the section above).

## DISTINCT INLINE CHAINS - DONE (2026-07-30), corpus 11 -> 9

The fix turned out to be ONE store, not the two mechanisms scoped below:
`emit_exc_stamp` already ran at every conveying exit **including the
emitted sync call**, so making it bake the op's chain index next to the
caret it already bakes covered BOTH cases at once. Concretely:

  * `Exception::jit_inline_frame` (int32, -1 = "look it up by pc"), stamped
    by the fragment on its cold failure branch with one
    `mov dword [rax+off], imm32`, guarded like the caret (skip when the
    object is null - a bail/eptr - and when already set, so the first
    conveyor wins). `emit_exc_stamp` was restructured so the chain store
    still runs for an exception that already carries a CARET but whose
    frames are not yet emitted, and so an op with a chain but no loc entry
    is still stamped.
  * `vm_flush_inline` prefers it over `Chunk::inline_frame_at(pc)`.
  * The deletability guard's distinct-chain check is GONE.

PROVEN, not assumed. The test shape puts the throwing call in the chain
listed SECOND, so the collapsed pc resolves to the FIRST one: instrumenting
the flush on that program printed `baked=1 pc=0 pc_lookup=0`, i.e. the pc
lookup WOULD have named the wrong inlined body and the baked index named
the right one. Pinned in `jit_final_batch_deletable`: the two engines'
backtraces must match, the frame must be `snd` and never `fst`, and
`g_jit_inline_baked` must have been bumped - so the test cannot pass on
luck (two engines agreeing because the correct body happened to be first)
nor on an unexercised path.

Effect: fib's two runs delete - `fib$0` is 9 instructions - and the corpus
is 9 kept runs, ALL of them the deferred catch dispatch. A throw through
fib's unrolled body stays byte-identical across engines.

WHY THE ESTIMATE BELOW WAS WRONG: it assumed the call case needed its own
route (a stub-pc `inline_ctxs` entry, or a field on the call record) because
`vm_unwind_walk` flushes at `ret_pc - 1`. That flush is guarded by
`inline_origin_emitted`, and the emitted call site's own exc-stamp runs
FIRST, so the baked chain is already in place by the time the walk looks -
no second mechanism, no touching the remap pass or the record.

## #78 steps 1-2: the battery's findings + the per-region state (2026-07-30)

Recorded in CLAUDE.md (the #78 paragraph). Corpus-visible: the deleted-
handler-pc HANG fix + the per-try-region pend/exc state (myv v7).

### Step-2 perf decomposition (callgrind, CLEAN same-flags builds)

The pend-state change measured, base = step 1 (763d30f):

    72_exc_finally   36,296,545 -> 36,798,579   +1.38%  = +1 Ir/iter
    42_exceptions   245,180,886 -> 251,191,026  +2.45%  = +30 Ir/iter

(42's per-iteration figure was first written as +20 by dividing by the
PRINTED `caught` (300000 = 100k*1 + 100k*2), not the loop count: 42 runs
N = 200000*scale iterations, one throw each. Corrected here and used
consistently below.)

72's ENTIRE delta is one instruction: the region dword store in the
native inline PushHandler. SetPend/EndFinally net ZERO - the new pends
indexing was paid for by switching their emitted rec addressing to the
activation's cached top_rec pointer (which also made them cheaper than
the old records+rec_n dance; the first cut without that was +12.4%).

42's +20/iter, per-function (self-cost diff):
    ~+10  the per-region indexed addressing where a fixed record field
          used to be: pend_at's vector index (+5.3 attributed to the
          return line) + the default build's _GLIBCXX_ASSERTIONS bounds
          check on operator[] (+3.3) + the 8-byte VmHandler copy - paid
          in the INTERPRETED CatchTest and the dispatch park
    ~+4   vm_dispatch self growth (the CatchTest handler's new shape)
    ~+2   vm_raise / the unique_ptr park-and-free churn
    ~+1   PushHandler's region store
    rest  fragment-layout attribution noise (nets ~0)

WHY THIS IS TRANSITIONAL, not the end state (the maintainer's principle:
an efficient nativization must not add net instructions): every line of
42's delta except PushHandler's +1 sits in machinery the remaining steps
DELETE or bypass -
  - step C/D move the match into vm_dispatch_exc and DELETE CatchTest/
    Reraise: the interpreted round trip (operand decode + dispatch +
    pend_at re-read) goes away; the match+bind work remains but as one
    direct C++ call on the raise path;
  - the handler table makes "does any catch body rethrow?" a STATIC
    per-site flag, so the park (the unique_ptr move + free) is SKIPPED
    for the overwhelmingly common no-rethrow site;
  - step D can drop SetPend(normal) from the normal path entirely
    (PushHandler resets the region's pend), which on 72's shape is
    roughly -8 Ir/iter against the +1 paid here.
PROJECTION: both benches end BELOW the pre-#78 baseline; measured at
each step against these exact numbers.

### Step A + B (2026-07-31): the shared matcher + the handler TABLE

A: the CatchTest handler's match logic became `vm_catch_match(chunk,
types_idx, exc)` - the one function the table dispatch will call, so the
two cannot drift while both exist.

B: `Chunk::handler_sites` - per try REGION, the clause list
({types_idx, bind_slot, body_pc}), the shared `fin_pc` (-1 = none), and a
`has_rethrow` flag. PRIMARY data (step D deletes the chain it could be
derived from), so it is SERIALIZED (myv v8, documented + doc-checker
extended) and REMAPPED everywhere a pc moves:
  - the peephole: jump THREADING, the branch-target map (a table pc is an
    entry - a no-op today since the CatchTest targets mark the same pcs,
    load-bearing at step D), and the compaction prefix-sum;
  - JIT remap 1 (the M3/M4 container path): plain `remap`;
  - JIT remap 2 (the delete-originals rebuild): `entry_remap`, the map
    CatchTest's target already takes (#74 inc 2) - a body pc is a RESUME.
`peephole_chunk` takes the chunk non-const now (it mutates the table).

THE SAFETY NET: `verify_handler_sites` (ASSERTS-only) re-walks each
PushHandler's chain and asserts the table still describes it exactly -
after codegen and after BOTH JIT remaps. It earned its keep immediately:
the first run aborted because the peephole DELETES the no-match `Jump`
when the shared finally is the next pc (E3's jump-to-next), a chain shape
my walk did not model - the table was right, the checker was wrong.

`-vd` renders the table; a new test pins its CONTENT (clause counts,
finally presence, has_rethrow, body pcs in range) over five shapes,
including that region ids follow COMPILE order, so an outer try holds a
LOWER id than a try nested in its body.

### Step C (2026-07-31): the raise path dispatches from the TABLE

`vm_dispatch_exc` no longer parks the exception and jumps to the
CatchTest chain. It now OWNS the whole same-frame decision: pop the
handler, index `handler_sites[region]`, run `vm_catch_match` over the
clauses, bind (`bind_slot`), PARK ONLY IF `has_rethrow`, and set `pc` to
the winning `body_pc`; on no match, `fin_pc >= 0` parks a `reraise` and
resumes in the finally, else it keeps walking OUT to the next enclosing
handler in the same frame before returning false to the frame walk.

CatchTest/Reraise are still EMITTED (step D deletes them) but are no
longer EXECUTED - nothing jumps to them.

Two things had to move with it:
  - the JIT ENTRY SET is now sourced from the handler table (every
    clause's `body_pc` and every `fin_pc`), not from CatchTest's target.
    Without the `fin_pc` half a throw resumed at a pc INSIDE a deleted
    run, collapsed onto the run's head EnterNative - the step-1 hang,
    resurfacing from the other side.
  - `vm_catch_match` is ML_ALWAYS_INLINE. Left out-of-line it cost
    +40 Ir per throw on 42 (258.4M vs 253.9M) - a call for two loads and
    a pointer compare.

FORM: ONE out-of-line `vm_dispatch_exc`, called from vm_raise and the
boxed-throw sites. Two leaner-looking alternatives measured WORSE and
were reverted:
    inline at the 5 call sites   42 = 257,460,346  (+3.6M)
    cold-tail split inside it    42 = 256,560,736  (+2.7M)
The first is the loop-body TEXT rule again (3 of the sites are inside
`vm_dispatch`); the second pays a call + argument marshalling for a
frame it barely shrinks.

### The instruction-count question, answered with the full chain

The maintainer's standing objection - an efficient nativization must not
ADD instructions - measured across every #78 commit, one session, clean
same-flag OPT=1 builds (42 = 200k throws, 72 = 500k iterations):

    commit            42_exceptions    /throw     72_exc_finally   /iter
    3dc7118 pre-#78    245,249,461         -        36,356,944        -
    763d30f step 1     245,180,788      -0.3        36,296,404     -0.1
    067cbfc step 2     251,190,917     +30.0        36,798,434     +1.0
    bc002f6 step B     252,233,683      +5.2        36,893,036     +0.2
    HEAD    step C     253,860,801      +8.1        36,935,079     +0.1
                                       -----                      -----
                                       +43.1                       +1.2

So the FLIP ITSELF (step C) is +8 Ir/throw, and the bulk of the
regression is NOT the nativization at all:

  step 2 (+30/throw) is the CORRECTNESS fix - the per-try-region state
  that closed 3 real clobber bugs. Its cost is almost entirely
  `vm_dispatch` SELF growth: +7,200,004 Ir = +36/throw, i.e. MORE than
  the step's whole delta (the rest nets -1.2M). That is the region
  operand widening the exception op cases in the dispatch switch - the
  front-end/code-layout tax, not work being done.

  step B (+5.2/throw) does NOTHING at runtime (it only BUILDS and
  verifies the table at compile time, on a 2-chunk program) - pure
  layout drift from the added code.

WHERE IT COMES BACK (step D, the next commit): deleting CatchTest and
Reraise removes exactly the two dispatch-switch cases that step 2 grew,
and PushHandler drops its target pc. That is the mechanism for the +36
and the +5.2 - the two layout items - and it is testable, not a hope:
if `vm_dispatch` self does not fall back to ~its step-1 value, the
claim was wrong and gets recorded as wrong.

Also still on the table for D: PushHandler resets its region's pend, so
`SetPend(normal)` can leave the normal path entirely (~-8 Ir/iter on
72's shape).

Residual after that, by construction: `vm_dispatch_exc` self is
116 Ir/throw, of which 32 are in nested-vector addressing
(`handlers`/`handler_sites`/`clauses`/`catch_uids`, three levels of
indirection). Flattening the clause list into one chunk-level vector
with (start,count) per site is the obvious next cut - deferred, since it
changes the myv layout again and step D must land first.

### Step D (2026-07-31): CatchTest and Reraise DELETED

D1 stops emitting them; D2 removes the opcodes. What went with them:
  - PushHandler's target pc (it carries only its region id), so it left
    `visit_pc_fields`, `op_is_branch` and `branch_pc_target`, and its
    emit moved from emit_branch to emit_op;
  - `VmHandler` is a bare 4-byte region id again, so the emitted inline
    push/pop store/step 4 bytes and the handler_base bake shifts by 2;
  - the "no clause matched" bytecode exit (`Reraise`, or SetPend(reraise)
    + Jump Lfin): vm_dispatch_exc does that itself now, so a plain
    try/finally emits ONE SetPend where it used to emit two;
  - the step-1 entry-stub fix for PushHandler's target - the handler-table
    entry loop is its general form;
  - `verify_handler_sites`' table-vs-chain walk, which has no second
    description to compare against any more. It was rewritten as the
    REMAP net it was really for (every table pc in range, every region
    resolvable, no empty site), still run after codegen and both remaps.
  - myv v9: deleting two opcodes renumbers the rest.

THE LOAD-BEARING NEW ROOT: the peephole's reachability DFS seeds from the
handler table. A catch body's only predecessor was the CatchTest jumping
to it; with that gone the DFS would call every catch body dead and DELETE
it. (Threading, the branch-target map and the compaction remap already
took the table into account from step B.)

STRUCTURALLY THIS IS THE GOAL: with no matcher pc to dispatch into, a
try/catch region DELETES like any other run. 70_exc_runtime_error's main
chunk goes from 35 ops to **2** (`enter.nat` + one resume `enter.nat`),
42_exceptions' to 3. The corpus audit (MYLANG_DELAUDIT=1 over bench/ +
samples/) goes from 9 kept runs to **1** - every one of the 9 was the
catch dispatch. The single survivor is 72_exc_finally's, blocked by
`EndFinally`, whose cold RERAISE arm still BAILS (it needs vm_raise's
dynamic resume); making it convey is step E.

(An earlier count here said ZERO. That was a measurement error, not a
result: the audit prints from jit_compile_chunk, and I ran it under
`-nr`, which never reaches the JIT - so "0" meant "nothing ran". The
numbers above are from real runs. This is the prove-the-code-ran rule
biting on the very claim the step is judged by.)

### Step E (2026-07-31): EndFinally conveys - the corpus reaches ZERO

`jit_end_finally(region, pc, inline_chain)` runs the interpreted op's
exact reraise body (the shared vm_raise) and reports like `jit_throw`:
0 = dispatched (the handler pc parked in g_vm_resume_chunk/pc and
returned as an ordinary external exit - the op ALREADY ran, so this is a
resume), 1 = boundary, 2 = conveyed, 3 = nothing pending (fall through).
The HOT normal arm is untouched - still an inline byte compare falling
through - and only the cold arm calls out, so the shape that made
EndFinally eligible in the first place is preserved.

`inline_chain` is `inline_frame_at(old_pc)` resolved AT COMPILE TIME and
stamped onto the exception before the raise: a deleted run's pcs have
all collapsed onto the head EnterNative, where vm_raise's own pc lookup
cannot discriminate between two inlined bodies (the same bake a
conveying fragment does - Exception::jit_inline_frame).

With no bail left, EndFinally is op_fully_native. **The delete-originals
corpus audit over bench/ + samples/ is now 0 kept runs** (72_exc_finally's
main chunk: 2 ops), verified with REAL runs. Perf is neutral by
construction (72 -1.41%, unchanged from step D: its hot loop never takes
the reraise arm).

TRAP: the cold arm is far past a short jump's reach - `emit_exc_stamp`
alone exceeds it - so the normal-path and nothing-pending jumps are
rel32. The first build asserted in `Emitter::patch8`.

Execution-proven by `g_jit_end_finally_reraise`, bumped only inside
jit_end_finally, asserted by a `jit:` test over all three outcomes
(same-frame dispatch, a CALLER's catch via the native walk, and the
boundary conversion of an uncaught reraise) plus a loop shape that
re-enters the region 5 times. A green suite alone could not tell the
native arm from the interpreter re-running a kept original.

### PERF: the step-C projection was WRONG, and here is the real story

I projected that deleting the two dispatch-switch cases would return
step 2's +36 Ir/throw of `vm_dispatch` text growth on 42_exceptions, and
committed to recording it as wrong if it did not. **It did not**: D2
returned 0.5M of the 7.2M.

Full chain (callgrind Ir, one session, clean same-flag OPT=1 builds;
42 = 200k throws, 72 = 500k iters, 69 = 20k throws x 17 frames,
70 = 200k throws):

                   42_exc      72_fin      69_cross     70_runtime
    pre-#78     245,249,461  36,356,944  327,486,167   72,179,755
    step 1      245,180,788  36,296,404
    step 2      251,190,917  36,798,434
    step B      252,233,683  36,893,036
    step C      253,860,801  36,935,079
    step D1     253,609,197  35,881,073
    step D2     253,068,587  35,840,484  348,347,014   80,333,384
    + reject    254,268,207  35,840,208  319,906,087   81,533,082
    + do/while  252,868,134  35,840,159  319,766,236   80,133,016
    vs pre-#78       +3.11%      -1.42%       -2.36%      +11.02%

Two fixes landed inside step D from these measurements:

  THE FAST REJECT. The frame WALK calls the dispatch once per POPPED
  frame, and almost every one of those frames has no live try. Pre-#78
  the dispatch was two loads and a compare, so it inlined and the reject
  was free; the table version is a real function, and paying a call
  frame per walked frame cost 162 Ir per frame - **+6.4% on
  69_exc_crossframe**. Restoring the emptiness test as an
  ML_ALWAYS_INLINE wrapper over an ML_NOINLINE loop took 69 from +6.37%
  to -2.36%.

  DO/WHILE. With the wrapper proving the first iteration's condition,
  repeating it in the loop head cost +6 Ir per throw on the same-frame
  benches (42, 70); the loop is a do/while.

WHERE THE RESIDUAL IS, split by the JIT kill switch (`-nj`):

                  JIT off (design)   JIT on (shipped)
    42_exceptions      +1.41%             +3.11%
    70_runtime         +4.15%            +11.02%

  - The DESIGN cost (+1.4% / +4.2%) is `vm_dispatch_exc_frame`: 89-116 Ir
    per throw where the interpreted CatchTest chain cost ~50. Roughly 32
    of those are NESTED-VECTOR addressing - `handler_sites` holds a
    `vector<HandlerClause>` per site and `catch_uids` a
    `vector<const UniqueId *>` per clause, so a match walks three levels
    of indirection. Flattening both into chunk-level arrays with
    (start,count) per site is the next step, and is now unblocked (it
    was deferred behind step D because it changes the myv layout).
  - The JIT-shape cost (the other ~1.7 / ~6.9 points) is the delete
    itself: a loop that throws EVERY iteration now leaves and re-enters
    ONE big fragment per iteration (exit -> vm_raise -> dispatch ->
    EnterNative at the resume), where before it stepped through several
    small fragments with the try region interpreted between them. That is
    inherent to delete-originals on a throw-every-iteration shape - these
    two microbenches are the worst case for it, not the typical one (72
    and 69, which do real work per iteration, both IMPROVED).

### #80 (2026-08-01): `rethrow` is native - the exception set is complete

`Rethrow` was the last jit-INELIGIBLE exception op. An ineligible op is
not merely interpreted: it SPLITS the run it sits in, so one `rethrow`
kept its whole try region interpreted. Measured on the same program:

    with    rethrow:  24 bytecode ops, 4 fragments
    without rethrow:   3 bytecode ops, 3 fragments
    with    rethrow (after #80):  3 ops, 3 fragments

`jit_rethrow(region, pc, lep, inline_chain)` is jit_end_finally's shape
plus the caret restamp: take the caught exception out of the ENCLOSING
catch's region slot, stamp the RETHROW SITE's caret, run vm_raise;
0 dispatched / 1 boundary / 2 conveyed, never falls through. The slot is
non-empty by construction - a region whose catch bodies contain a
`rethrow` has `has_rethrow`, which is exactly when the dispatch parks the
exception there (a null would be a broken invariant, so the helper
conveys an InternalErrorEx rather than silently swallowing).

BOTH the LocEntry and the inlined-at chain are BAKED at compile time: the
interpreted op reads them via loc_at(pc) / inline_frame_at(pc), and a
DELETED run's pcs have all collapsed onto the head EnterNative.

The test pins the counter over three outcomes (same-frame catch, a
caller's catch via the native walk, boundary) AND checks CARET PARITY -
tw == vm, and the position must be the `rethrow` line. That second check
is the load-bearing one: a wrong bake leaves behavior perfectly correct
and only moves the reported error position, which no other test in the
suite would notice.

With this, every exception construct - `throw`, runtime errors, `try`/
`catch` entry and matching, `finally`, `rethrow` - is native.

### CORRECTION: every #78/#80 number above was taken at ASSERTS=1

The maintainer caught this. All the measurement lanes were plain
`make -j OPT=1`, which defaults ASSERTS=1; the perf gate is ASSERTS=0.
**An ASSERTS=1 delta cannot be extrapolated** - assertion cost is not a
uniform multiplier, it lands unevenly per code path (the per-op
ML_VM_CHECK tier, `_GLIBCXX_ASSERTIONS` on container access), so a change
that MOVES work between paths can measure better or worse purely because
of where the asserts sit.

Re-measured, pre-#78 (3dc7118) -> HEAD (f4edb53), ASSERTS=0 both sides:

                        ASSERTS=1   ASSERTS=0
    42_exceptions          +3.11%      +2.83%
    70_exc_runtime        +11.02%      +9.47%
    69_exc_crossframe      -2.36%      +0.22%   <-- SIGN FLIP
    72_exc_finally         -1.42%      -1.33%

69_exc_crossframe is the warning. Under ASSERTS=1 the fast-reject fix
looked like a 2.4% WIN; at ASSERTS=0 it is ~neutral. The win I recorded
was substantially the removal of hardened container access on the walk
path, not the call-frame saving I attributed it to. The other three hold
their direction, and the honest headline is: 42 and 70 regress somewhat
LESS than reported, 69 does not improve, 72 still improves.

#81 shows the same effect from the other side: ASSERTS=1 said +0.22%,
ASSERTS=0 said +1.14% for the identical change - a 5x difference in the
delta. That one is measured BOTH ways below.

### #81 - flattening the handler table: MEASURED, NEGATIVE, NOT LANDED

Parked on `wip/81-flatten-handler-table` (green: 1665/1665 + 1478/1478),
not merged. The premise was that matching a catch clause walked three
levels of dependent loads (table -> the site's clause vector -> the
clause's type-name vector), ~32 Ir per throw, and that one flat pool per
chunk with (base, count) per site would remove them.

MEASURED vs f4edb53 (callgrind Ir), BOTH build configs:

                     A=1 jit-on  A=1 jit-off  A=0 jit-on  A=0 jit-off
    42_exceptions       +0.22%      +0.19%      +1.14%      +1.01%
    70_exc_runtime      +0.55%      +0.36%      +2.88%      +1.92%
    69_exc_crossframe   +0.01%      +0.03%      +0.04%      +0.07%
    72_exc_finally      +0.12%      +0.03%      -0.01%      -0.00%

At ASSERTS=0 - the config that ships and the one the gate uses - it is
FIVE TIMES worse than the ASSERTS=1 reading suggested, because the
NESTED baseline was the side paying the hardened container access;
remove hardening and the flat form's real extra work is all that is
left. The decline is not close.

Negative everywhere, after three rounds of tuning off a first cut at
+1.76% / +3.96%. The two real lessons from those rounds:

  1. **`operator[]` is NOT free in this build.** ASSERTS is on by default
     (release included), so `_GLIBCXX_ASSERTIONS` bounds-checks every
     `vector::operator[]`. Indexing the flat pool put a size() compare on
     EVERY element of the matcher scan, where the nested form paid one
     check on the outer lookup and then iterated unchecked: **+28 Ir per
     throw**. A flattening only pays if the hot scan goes through
     `.data()`.
  2. **A 12-byte element makes indexing cost a multiply.** `cls[ci]` on a
     3 x int32 struct needs an imul per access; the old range-for was a
     pointer increment. Worth another ~0.8% on 42.

WHY THE PREMISE WAS WRONG (the part worth remembering): the inner
vector's `_M_start`/`_M_finish` live INSIDE the site struct, which the
dispatch has already loaded - so the nested range-for is a plain pointer
loop with NO extra dependent load. The flat form must load
`handler_clauses.data()` (a different cache line of Chunk) plus
clause_base and n_clauses and then compute the base: strictly more work.
The indirection I set out to remove was never being paid in the first
place. "Nested vectors are three pointer hops" is true of the TYPE and
false of the generated CODE when the outer element is already in hand.

The residual +1.4/+4.2% interpreter-side dispatch cost from #78 is real;
it is simply not here.

## The original scoping (kept for the record - superseded above)

SCOPED 2026-07-29, before the implementation showed one store sufficed.

The guard keeps a run whose `inline_ctxs` entries name DIFFERENT chains,
because deleting it collapses every pc onto the head EnterNative and
`vm_flush_inline`'s pc-keyed `inline_frame_at` would then emit the WRONG
virtual frames in a backtrace. Both remaining runs are in fib's unrolled
body, and their raising ops are:

    run[0,50)   8x CachedCallV                     (calls only)
    run[0,40)   SubscriptV, CallBuiltinV, 4x CachedCallV

So a fix must cover BOTH a raise conveyed by a non-call op AND an
exception propagating THROUGH a call - a partial fix unblocks neither
run. Two mechanisms, both cold-path:

 1. NON-CALL conveying ops: bake the chain index the way the CARET is
    already baked. `emit_exc_stamp` writes the op's Locs straight into
    the exception object on the cold failure branch; one more store would
    write an `Exception::jit_inline_frame`, and `vm_flush_inline` would
    prefer it over the pc lookup. Zero hot-path cost, symmetric with the
    existing design.

 2. CALLS: `vm_unwind_walk` flushes at `cur.ret_pc - 1`, which is
    ambiguous once collapsed. The zero-cost route is the POST-CALL ENTRY
    STUB pc, which is unique per call site and already stored as
    `rec.ret_pc`: emit an `inline_ctxs` entry keyed by the stub pc during
    the delete/remap pass, and have the walk try `ret_pc` before
    `ret_pc - 1`. The alternative - baking the index into the call
    RECORD - is rejected: that puts a store on the emitted M5b push, i.e.
    the hot call path lever 1 fought to strip.

WHY IT IS NOT OBVIOUSLY WORTH IT: the benefit is ~90 interpreted
instructions of bytecode in ONE bench and the word "universally"; there
is NO runtime gain, since the native code runs either way and this is all
cold-path. The cost is ~100-150 lines touching the exception object, the
exc-stamp emitter, the delete/remap pass (which carries a
`ML_CHECK(pe == entries.size())` invariant and a dual remap) and the
unwind walk - the area with the most subtle past bugs (a stale caret, the
byte-identical backtrace pins, the fuzzer-caught codegen-field bug) - in
the hottest recursion bench. A parity test (throw through a distinct-chain
deleted run, backtrace byte-compared against the tree-walker) would be
mandatory.
