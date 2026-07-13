# VM performance roadmap: from 3.9x to 5x+ over CPython

Deep-dive (2026-07-15, HEAD 98d5d70, release `OPT=1 ASSERTS=0`, WSL2 —
wall clock via `bench/run.py`, deterministic counts via cachegrind /
callgrind; the machine has no PMU, so hardware-counter claims come from
cachegrind's simulator + the earlier real-CPU measurements recorded in
`vm-fallback-elimination.md`).

## Part 1 — WHY the geomean "regressed" from ~4.6x to 3.9x

Apples-to-apples reconstruction: the peak-era commit `238e8be` (2026-07-06,
the last before benches 63+ were added) was rebuilt and re-run TODAY on its
own 62-bench set; HEAD ran the full 77. The classic-62 scripts are
byte-identical between the two trees, so the same rows compare directly.

| set | binary | my/py geomean |
|---|---|---|
| classic-60 paired | PEAK 238e8be | 0.254 (3.94x) |
| classic-60 paired | HEAD 98d5d70 | 0.238 (4.21x) |
| classic minus 42_exceptions (59) | PEAK | 0.235 (4.25x) |
| classic minus 42_exceptions (59) | HEAD | 0.236 (4.24x) — **NET FLAT** |
| the 15 NEW benches (63..77) | HEAD | 0.456 (**2.19x**) |
| full 75 paired | HEAD | 0.271 (3.69x; run.py prints 0.26/3.9x) |

Conclusions:

1. **The drop is ~entirely SUITE COMPOSITION.** The 15 benches added since
   07-07 (closures, struct create, make_dict, dyn foreach, exceptions,
   unpack, funcval dispatch) geomean at **2.19x** vs the classic set's
   4.2x. Folding them in dilutes 4.2x -> 3.7-3.9x with zero code change.
   They were added deliberately — they cover the constructs the VM was
   weakest at — so the "regression" is mostly the yardstick getting
   honest. These 15 are also exactly where the 5x work is.
2. **On the classic set HEAD is NET FLAT vs peak — but that hides a real,
   broad dispatch regression.** 26 classic benches are >5% WORSE (the
   dispatch-bound loops: 01/04/05/06/07/43/44/45/53/54/60/61, sorts,
   builtins — worst: 53_collatz +32%, 06_if_branch +32%, 45_gcd +29%),
   compensated by 18 benches >5% BETTER (the nativization wins) plus the
   P8 exceptions win (42: 0.99 -> 0.02 my/py). Recovering the 26 without
   giving back the 18 is pure geomean upside (~+10%).
3. **The mechanism of the broad regression is the dispatch loop itself.**
   Since the peak era `vm_run_chunk`'s switch grew **43 -> 92 cases** and
   the binary text +19% (1.67 -> 1.99 MB). Cachegrind on UNTOUCHED loops:
   01_while_loop +4.5% Irefs and **+33% indirect-branch mispredicts**,
   60_bit_sieve +7.5% Irefs, 44_primes_sqrt +4.4% — the handlers didn't
   change; the giant function's register allocation and the single
   indirect-branch hub degraded as cases were added. This is the same
   effect measured on real hardware earlier (~12.5% wall on untouched int
   loops from +0.5% instructions — see `vm-dispatch-frontend-regression`).
4. **Residual peak-print delta (4.2x re-measured today vs the ~4.5-4.6x
   remembered/reported then), same code:** the CPython *baseline* moved —
   the bench python is now brew CPython 3.14 (tail-calling interpreter);
   wall-clock drift on WSL2 adds noise. The MyLang binary at 238e8be is
   bit-identical to what it was; only the denominator changed. Treat
   3.94x/4.21x (today's numbers, today's python) as the truth going
   forward.

## Part 2 — where the time actually goes (profiles)

- **35_map_filter `-vm` (VM 25% SLOWER than the tree-walker):**
  `do_func_call` + its inlined parts = **~30% of ALL instructions**;
  `EvalContext::EvalContext` alone 4.7% (a 136-byte context built PER
  CALLBACK CALL, including a `std::map` member); the callback body chunk
  (`vm_run_chunk`'2) only ~14%. The tree-walker wins because its
  expression-body fast path evaluates `=> x*2` directly — no chunk
  dispatch, no FlowState round-trip. The VM pays full call machinery for
  a 3-op body, per element.
- **13_array_append `-vm` (VM 26% slower than TW):**
  `vm_call_builtin_lv_rest` (the rest-arg marshaling wrapper) = **~23%**;
  `LValue::get_value_for_put` (the per-append COW/alias check) 7.4%;
  `builtin_append` itself 7.3%. The TW's `DirectBuiltinCallExpr` evals
  args in place — no rest-run copy, no boxed re-copy of the array handle
  (`TypeImpl<SharedArrayObj>::copy/move` ~5% under the VM).
- **Hot-op decode:** `IntBin` executes a SECOND 11-way switch on `in.aop`
  per instruction; every operand read branches on `is_lit`.
  `sizeof(Instr) = 56` bytes (2x16-byte Operands + fields) — Lua's is 4.
  `sizeof(EvalContext) = 136`, `LValue = 48`, `Frame = 432` (+ per-call
  `Frame::init` default-constructing 48-byte LValues).
- **Worst absolute my/py at HEAD:** 69_exc_crossframe **1.88** (the only
  bench meaningfully SLOWER than CPython — cross-frame throws still pay a
  landing-pad + signal plumbing per frame), 67_make_dict 1.24 (callback
  per key), 76_funcval_dispatch 1.20, 34_sort_custom_cmp 1.13 (callback
  per compare), 13_array_append 0.96, 35_map_filter 0.96,
  31_str_split_join 0.83, 10_recursion_deep 0.77 (calls ~ TW parity).
- **VM SLOWER than the tree-walker at HEAD** (vm/tw > 1): 67_make_dict
  1.29, 13_array_append 1.26, 35_map_filter 1.25, 76_funcval 1.20 (noisy,
  ~0.9-1.2), 34_sort_custom_cmp 1.11, 31_str_split_join 1.00. ALL are
  call/builtin-marshaling-bound — the VM's per-call overhead exceeds its
  dispatch win there.

**The geomean math** (unchanged from the earlier analysis): reaching 5x
(0.20) from 3.9x (0.256) needs the PRODUCT of all improvements to be
~0.78. No single fix does that: it's ~2x on 12-15 laggards, or ~15%
broad + ~1.7x on 10. The plan below is sized accordingly.

## Part 3 — the opportunity catalog

### A. Dispatch engine (broad; recovers the known regression)

- **A1. Computed-goto / direct-threaded dispatch — ✅ DONE (2026-07-16),
  measured A/B.** GCC/clang dispatch via a `&&label` table (generated in
  enum order from ML_FOR_EACH_OPCODE, order/coverage static-asserted) with
  the dispatch at each handler's tail; `make CGOTO=0` / MSVC keep the
  switch (same bodies via VM_CASE/VM_NEXT). A/B (same commit, CGOTO=1 vs
  0, scale 10 best-of-7): **-10.4% geomean over the 15-bench dispatch set**
  (01 -22%, 07 -22%, 44 -21%, 06 -18%, 54/61 -17%, 60 -1%; call-bound
  fib/map_filter/dict_insert ~flat as expected). Cachegrind: instructions
  -11-12%, indirect-branch mispredicts -25-42% on untouched loops (44:
  87.5M -> 51.1M, BELOW the peak-era 94.7M). Suite: my/py 0.26x -> 0.25x
  (run.py: 3.9x -> 4.0-4.2x across runs), VM/TW 0.56x -> 0.53x. Two
  portability rules learned: an INDIRECT goto may not exit a scope with
  live destructors under clang - so every handler's terminal dispatch sits
  AFTER its case braces, and the 3 cold cross-frame-exception sites use a
  direct-goto trampoline (VM_NEXT_COLD); and in switch mode VM_NEXT must
  be `continue` (a `break` inside a handler's inner switch would fall out
  into the slot write - caught by -Wmaybe-uninitialized under LTO).
  Bonus: the switch-mode refactor itself measured ~4% FASTER than the
  old HEAD on the loop set (favorable layout from the `in->` pointer
  form), so the total recovery vs pre-change is ~14% on those benches.
- **A2. Hot/cold handler split — ONLY together with A1, never alone.**
  HISTORY (2026-07-08, transcript + `vm-dispatch-frontend-regression`
  memory): the standalone cold-split experiment (10 fat ops into an
  `ML_NOINLINE vm_cold_op`) DID shrink vm_run_chunk 6952 -> 5803 static
  instructions yet made the regression WORSE (NEW/OLD 1.168 vs 1.125) -
  measured, reverted. Lesson: perturbing the one-big-switch layout in
  isolation is a coin flip. Under COMPUTED-GOTO the calculus changes
  (each handler is its own block with its own dispatch tail; keeping the
  hot ones small is then a local property, not a global-layout gamble),
  so do the split as part of the A1 conversion and A/B the pair.
  ALSO FALSIFIED (2026-07-08/09, "Revert to the switches"): converting
  the big switches to `.rodata` array lookups - the audit showed the hot
  dispatch switches are ALREADY jump tables (`notrack jmp *%rax`), and
  the value-map conversions (binop_pmf/cmp_pmf) measured ~2-3% SLOWER.
  Computed-goto is NEITHER of those: it was explained + tracked as the
  big lever on 2026-07-10 but never implemented.
- **A3. Hot-case ordering + pc-as-pointer.** Keep the hot opcodes first
  in the enum (jump-table locality); iterate `const Instr *ip` instead of
  `code[pc]` indexing (saves an index-scale per op; pc reconstructed only
  on the cold error path via `ip - code.data()`).

### B. Instruction set & encoding

- **B1. Split IntBin/FloatBin by operator.** `IntAdd/IntSub/IntMul/...`
  (and float twins) remove the per-execution inner 11-way switch on
  `aop`. (That inner switch IS already a jump table - the .rodata audit
  proved the compiler does that - but it is still a second data-dependent
  indirect branch + table load per arith op; a per-op opcode turns it
  into straight-line code.) With A1 each becomes a tight 3-address
  handler. Div/mod keep their zero checks in their own handlers (the hot
  add/sub/mul lose the branch entirely).
- **B2. Immediate-variant opcodes.** `IntAddRR/IntAddRI` (reg-reg,
  reg-imm) etc. remove BOTH `is_lit` operand-decode branches from the hot
  path; the codegen knows the shape statically. Include `ModConstI`
  (`s % 1000000007` appears in nearly every bench's checksum loop) and
  `AddI dst, src, #1` (i++ shapes outside ForLoopStep).
- **B3. Shrink `Instr` 56 -> 32 bytes — DONE (2026-07-17, stage 1).
  MEASURED (full-suite interleaved A/B vs 94e84f7): VM-wall geomean
  **0.970** — a broad −2-4% (well ABOVE the "low single digits at
  best" estimate below; the D-cache effect is real), my/py 4.57-4.59x
  → **4.67-4.68x** on both runs. Implementation notes: `Operand`
  survives as the codegen-side value type; `Instr` stores packed
  payloads + a shared flags byte behind accessors (`a_slot()`/
  `a_lit()`/`a_flit()`/`a_is_lit()`/`a_kind()`, `set_a()`, and the
  unpacking `a()` for pass-by-const-ref sites); `static_assert(sizeof
  (Instr) == 32)`. SEVEN ops (the CallBuiltinLV family incl. AppendV +
  the chain stores) used the fat Operand's slot AND lit as TWO
  independent fields at once — they now use the DUAL payload view
  (`set_a_dual(lo, hi)` / `a_dual_lo()` / `a_dual_hi()`, int32 halves).
  TRAP FOR THE RECORD: the mechanical regex rewrite corrupted MyLang
  TEST-SOURCE STRINGS in tests.cpp (`d.a = 5;` → `d.set_a(5);`,
  `f(d.a)` → `f(d.a())`) — when regex-editing C++ that EMBEDS MyLang
  code, audit string literals explicitly. Stage 2 (the CgInstr
  node_idx split, below) is the architectural follow-up. Original
  design analysis kept below for reference.** Measured today:
  `sizeof(Instr) == 56` (OpCode is already 1 byte; `Op aop` is 4, needs
  1; each 16-byte Operand carries 9 bytes of information — the 8-byte
  lit/flit union forces alignment padding around the 2 tag bytes + the
  4-byte slot).
  **The packing (two observations):** (1) `slot` and `lit` are MUTUALLY
  EXCLUSIVE (`is_lit` discriminates) — slot moves INTO the 8-byte
  union; (2) the per-operand tag bits (`is_lit` + `lit_kind`, 2 bits
  each) hoist into ONE shared flags byte on Instr, and `aop` becomes
  `enum Op : unsigned char`. Result: op(1) aop(1) flags(1) pad(1)
  target(4) target2(4) node_idx(4) a_payload(8) b_payload(8) = **32
  exactly, zero pad waste** — two instructions per cache line instead
  of one straddling two.
  **Mechanics:** do NOT hand-edit the hundreds of `in->a.slot` /
  `a.is_lit` consumer sites — `Instr` gains accessors (`a_slot()`,
  `a_lit()`, `a_flit()`, `a_is_lit()`, `a_kind()`, b-twins) and the
  consumers rewrite one-for-one with no logic change. The hazard class
  is E-v1's (a mis-mapped field = silent corruption), so it ships only
  with the full fuzzer + differential + matrix treatment.
  **`node_idx` removal (analyzed with B3, 2026-07-17):** the field is
  the codegen-transient splice-stable AST handle (extract_locs /
  verify_ast_free), 4 dead bytes at runtime. The clean removal is a
  CODEGEN-ONLY subclass — `struct CgInstr : Instr { int32_t node_idx; }`
  — codegen works on `vector<CgInstr>` (field accesses compile
  unchanged via inheritance; ~50 signatures change), the peephole +
  extract_locs run on it, and codegen_chunk's tail SLICES the Instr
  sub-objects into the chunk. Type-level zero-AST enforcement (the
  runtime Instr structurally cannot reference the AST) + `.myv` hygiene.
  BUT the size arithmetic says it's FREE PADDING under the 32-byte
  pack: the packed header with node_idx is exactly 16; without it, 12
  pads back to 16 for the 8-aligned payloads — STILL 32. Going below 32
  needs int16 target/target2 (overflow guards; a 24-byte stride
  straddles lines every ~8th instr) — dubious marginal win. So: do the
  CgInstr split WITH B3 as the architectural cleanup, not for bytes.
  **Honest payoff estimate:** a D-cache/bandwidth effect on the
  bytecode stream; hot loop bodies (5-15 ops) fit L1 at either size, so
  the win concentrates in big chunks / call-heavy chunk switching /
  image size. Expect low single digits geomean AT BEST, plausibly ~0 —
  the same magnitude as layout noise, so it needs pooled interleaved
  A/Bs to even resolve. Also a `.myv` prerequisite-ish cleanup (smaller
  serialized files, no dead field).
- **B4. More fused superinstructions, chosen from PROFILES not vibes:**
  compare+branch for BOXED conditions (`CmpV`+`JumpUnlessTrueV` pairs),
  `LoadElemInt`+`IntBin` (a[i] feeding arith — the sieve/matrix inner
  shape), `StoreElem`+`ForLoopStep` back-edge fusion. The earlier
  superinstruction study (tree-walker) failed because dispatch was
  per-NODE; in the VM each fusion genuinely removes a dispatch.

### C. The call protocol (the single biggest structural item)

- **C1. A native in-VM call stack for chunked callees.** Today EVERY call
  — even VM->VM — goes through `do_func_call`: build a 136-byte
  `EvalContext` (with a `std::map` member), link capture_ctx, init a
  Frame, bind, re-enter `vm_run_chunk` via C++ recursion. Profiles: ~30%
  of map_filter, ~half the per-call cost of fib. Replace with a VM frame
  stack: `CallV` pushes {return pc, chunk, frame base} onto a preallocated
  stack, binds params register-to-register (the arg run is ALREADY a
  contiguous slot window — VmArgs), and jumps; `ReturnV` pops. No
  EvalContext, no C++ recursion (also fixes the Windows/ASan stack-depth
  class), no FlowState round-trip. The tree-walker path and builtin
  callbacks keep do_func_call; the boundary (a VM frame calling a builtin
  / TW closure) converts explicitly. This is a big, staged project — but
  it is where fib/recursion/closures/higher-order/funcval (a dozen
  benches, incl. most of the NEW low performers) double or better.
- **C2. Callback-loop frame REUSE (cheap precursor to C1).** map / filter
  / sort-cmp / make_dict / find-keyfunc call the SAME callee N times:
  build the args_ctx + Frame ONCE, rebind the param slot per element, and
  rerun the body chunk (the frame is dead between iterations by
  construction — same soundness as the foreach loop-var reuse). Kills the
  per-element EvalContext+Frame cost that makes the VM SLOWER than the
  tree-walker on 34/35/67 today. Contained inside vm_map_filter /
  sort_core / make_dict.
- **C3. Tiny-body call inlining at CODEGEN.** A callee chunk that is just
  `{Load*, IntBin, ReturnV}` (the `=> x*2` class) can be spliced into the
  CALLER's chunk at a CallV site with a register rename — the VM
  equivalent of the AST inliner, catching what the AST inliner declined
  (indirect callees can't; direct tiny calls it already ate). Needs C1's
  frame model OR a temp-window convention. Evaluate after C1/C2 — they
  may make it unnecessary.

### D. Builtin ABI marshaling

- **D1. Append/push fast op. DONE (2026-07-16).** `vm_call_builtin_lv_rest`
  was ~23% of 13_array_append. `AppendV` (shares CallBuiltinLV's operand
  layout, selected in the codegen when the rest-native callee is
  `append`/`push` with exactly one value arg): arg0's LValue* formed from
  the slot, the VALUE read straight from its register, and the never-
  throwing shared core `arr_append_fast` (arr.cpp.h — flat int/float/
  bool/POD-struct/general + incremental hash maintenance; `builtin_append`
  is refactored over the same core, so both engines share one append) run
  inline — no rest-run copy, no ArgLocs deref, no builtin fn-pointer call.
  Any decline (const/readonly/slice/flat-mismatch/non-array/undefined
  global) falls to the full `vm_call_builtin_lv_rest`, byte-identical.
  MEASURED (full-suite interleaved A/B vs b1b2): VM-wall geomean 0.983,
  13_array_append 0.062→0.051s (0.82x), suite 4.49-4.50x CPython.
- **D2. Pass builtin value-args as a slot WINDOW, not copies.**
  `CallBuiltinV` copies each arg EvalValue into a buffer; a `const
  LValue *args + n` view over the frame run (exactly VmArgs) removes a
  refcount bump + 32-byte copy per arg. Needs the func_v ABI widened to
  the view type — mechanical but touches every migrated builtin.

### E. A post-codegen optimizer (the "LLVM pass" the maintainer invited)

**DONE (2026-07-16) — `peephole_chunk` (codegen.cpp), design + field
tables in `plans/vm-peephole.md`.** Runs BEFORE `extract_locs` (so the
loc/inline_ctxs side tables build from the compacted code — no side-table
remap, only Instr pc fields), iterated ≤4 rounds. MEASURED (full-suite
interleaved A/B vs 6ee507c): **VM-wall geomean 0.987** (a broad −4-14%
across ~17 benches; my/py 4.41-4.44 → 4.45x), bench+samples instrs
3761→3587 (−4.6%), MoveVs −31%, fib$0's chunk 68→56 (−18%). Fuzzer
400/400, matrix green.

- **E1. Copy propagation / MoveV elimination — DONE.** Backward temp
  liveness (single-word bitset over `[slot_count, +n_temps)`, barrier
  ops read all temps, handler pcs absorbed into every op's live-out)
  proves the arm temp dead; `<producer dst=tX>; MoveV d=tX` (adjacent,
  no branch entering the move) retargets the producer to d and deletes
  the move. The USE/DEF table (`visit_use_def`) and the retargetable-dst
  whitelist are AUDITED per op against disasm/vm.cpp — an unlisted op is
  a conservative barrier, never a guess.
- **E2. Dead-temp elimination / temp reuse — EVALUATED + DEFERRED.**
  The native call stack already made per-call temp cost ~nil (an in-VM
  window push constructs no slots; pop resets by scanning content, not
  by n_temps; boundary Frame::init is rare) — full dense renumbering
  needs the complete slot-field visitor for a ~nil measured win. See
  the plan file.
- **E3. Jump threading — v1 TRIED + DECLINED (2026-07-16).** An
  in-place `thread_jumps` pass (retarget every branch pc-field through
  chains of plain `Jump`s, hop cap 8, no instruction deletion) was
  implemented, debugged, validated (full matrix + fuzzer 300/300), and
  MEASURED — then reverted. Two findings, both permanent lessons:
  1. **The benefit potential is ~zero.** The codegen already emits
     direct branches: across ALL 77 benches the pass retargeted
     instructions in exactly ONE program (68_nested, 24 instrs; every
     other bench's dump was byte-identical). Jump-to-jump chains barely
     exist in real chunks, so threading alone cannot pay for anything.
  2. **The cost was a consistent +3.2% VM-wall suite-wide** (interleaved
     full-suite A/B, 2 runs each: base 4.45-4.49x, head 4.38x twice) —
     smeared across benches the pass provably didn't touch, i.e. the
     known LTO/code-layout front-end effect from perturbing the binary
     (see the dispatch-front-end memory note), not the pass's runtime.
     Even 68_nested got slower (1.047x): the layout smear swamps 24
     saved dispatches.
  Verdict: threading only makes sense INSIDE the full E1-E4 peephole
  (instruction deletion + pc remap), where the dead Jumps are actually
  removed and frames shrink — not as a standalone retargeting pass.
  **[RESOLVED: the peephole (see section E) now does exactly this —
  threading + jump-to-next + unreachable deletion + int branch-over-jump
  inversion, with compaction; measured 0.987 as part of the pass.]**
  **Correctness trap for that future pass (fuzzer-caught here, the
  differential suite missed it):** an Instr "target" field is NOT always
  a pc — `ForLoopStep::target2` is the COUNTER SLOT; threading it
  corrupted the loop var whenever the slot number equaled some Jump's pc
  (13/300 diverged, a Frame::at bounds abort). Verify each op's field
  semantics in its vm.cpp handler before treating it as a pc, and ALWAYS
  run nested_fuzz.py after a codegen-pass change.
- **E4. The fusion pass** — pattern-match adjacent ops into the B4
  superinstructions instead of hand-coding each shape in the codegen (one
  place to add patterns, codegen stays simple).

### F. Widen the typed (unboxed) tier

- **F1. Typed math-builtin calls — DONE (2026-07-16, `MathFnV`).** The
  40_math_builtins gap was NOT boxed arith (the loop's `f.bin` chain was
  already typed — the typed compilers route builtin results through temps)
  but the per-call `CallBuiltinV` marshal: a boxed `move` per arg into the
  run, the arg-buffer copy, the ArgLocs, the builtin fn-pointer call, the
  boxed result store. `MathFnV` (target2 = a `MathFn` selector) deletes all
  of it: `read_float_operand` (an int arg promotes, like FloatBin's), a
  direct libm call in the ML_NOINLINE `vm_math_fn` (loop-body text rule),
  `write_float_slot`. Selected by `try_math_fn` (codegen.cpp) ahead of the
  generic lowering, gated on: an unshadowed builtin, exact arity (wrong
  arity must throw → generic), th==f on the call (so `abs(int)` → int and
  `float("3")` → str-parse stay generic), float-compilable args. NEVER
  THROWS (the float builtins have no domain checks — libm NaN/inf — and
  arity/type errors are excluded at compile time), so loc- AND node-free.
  Covers sqrt/cbrt/sin/cos/tan/asin/acos/atan/exp/exp2/log/log2/log10/
  ceil/floor/trunc/float/abs-on-float + 2-arg pow. MEASURED (full-suite
  interleaved A/B vs 11e6d43): 40_math_builtins 0.030→0.015s (**0.50x
  VM-wall — my/py 0.42x → 0.19-0.20x, ~5x CPython**), suite VM-wall
  geomean 0.999 (the ±5-13% per-bench spread is layout jitter, netting
  to zero). The "typed MEMBER reads beyond dicts" residue of F1's
  original scope is DONE under H1 (LoadMemberInt/Float — and the "no
  bench is gated on it" claim was wrong: 64_struct_create was).
- **F2. Bool-typed conditions.** `while (flag)` boxes through
  JumpUnlessTrueV; a `JumpUnlessBool` on a proven-bool slot reads the
  byte directly.

### G. Exceptions endgame

- **G1. Cross-frame propagation without ANY landing pad — DONE
  (2026-07-17).** `vm_raise` (vm.cpp) now runs the native FRAME WALK
  (`vm_unwind_walk`) directly: dispatch in the current frame, else pop
  in-VM records (capturing their backtrace frames) until a handler or
  the activation's BOUNDARY record — only the boundary converts to the
  `g_vm_exc_pending` signal. NO C++ throw anywhere on the VM-raised
  path (the old shape C++-threw to the boundary catch whenever no
  SAME-frame handler existed — one landing pad + an exception CLONE per
  cross-frame raise). All six raise sites walk: `Throw`, `Reraise`,
  `Rethrow`, `EndFinally`'s reraise, and the IntBin/FloatBin div0
  pair; a dispatch that lands in another frame refreshes the cached
  `code` pointer before re-dispatching. The boundary catch remains for
  the type-system C++ throws the VM can't pre-detect (OOB /
  KeyNotFound / boxed TypeErrorEx) and now ALSO avoids per-frame
  landing pads (it always did, post-C1). Backtraces byte-identical
  (differential-pinned; the walk pushes the same frames the boundary
  path did, minus the clone). **The same-frame FAST PATH stays out of
  the cold walk** (vm_raise dispatches in the current frame first, and
  is itself NOT cold-marked): the first G1 shape routed same-frame
  throws through the ML_COLD walk and cost a MEASURED, persistent +12%
  on 42_exceptions (a cold-section call per throw) - restored, 42 back
  to 0.95x. MEASURED (full-suite interleaved A/B vs a2f16b1; the first
  round's +1.2% VM-wall read was machine drift - the drift-immune
  per-run my/py IMPROVED while wall "regressed", and a pooled
  best-of-4 showed 1.001 - the fixed version measured cleanly):
  69_exc_crossframe 0.055→0.031s (**0.564x; my/py 1.43x → 0.80x — the
  last CPython-losing bench now WINS**), 42_exceptions 0.947x,
  suite VM-wall geomean **0.985**, my/py 4.50-4.51x.

### H. Runtime / library-level (engine-neutral; caps several laggards)

- **H1. Struct creation — v1 DONE (2026-07-17): DST-SLOT REUSE.** The
  hot standalone shape is `var p = Point(...)` in a loop: the dst slot
  still holds LAST iteration's same-def POD instance, and a construction
  paid TWO heap allocations (the `StructObject` + its `bytes` vector)
  plus two frees per instance. `vm_struct_ctor` (vm.cpp) now constructs
  INTO the dst slot: when the slot's current value is a same-def,
  non-readonly POD instance with `use_count() == 1` (the slot's handle
  is the only owner), the fields are coerced into a stack buffer and
  written over ITS bytes — zero allocations in steady state. An aliased
  (`var q = p`), captured, const, other-def, or non-struct dst takes the
  fresh path, so a held instance is never mutated — the same
  overwrite-in-place + COW-guard trick the flat-struct-array foreach has
  always used (do_iter). Coercion happens BEFORE dst is touched (a
  defensive throw leaves the old value intact); the fresh path stores
  the pre-coerced bytes directly (no double coerce). Alias-soundness
  pinned by a dedicated test (both engines).
  **The MEASURED discovery: allocation was NOT the dominant cost.** The
  reuse alone A/B'd ~neutral on 64_struct_create — the dump showed the
  body was 5 `member.v` + 6 boxed arith per iteration: the FIELD READS
  were boxed, because the VM had no typed lowering for a standalone
  struct member (only the foreach-array LoadStructField* and the dict
  DictLoad* pairs; the F1 note claiming "no bench is gated on
  MemberIntV" was wrong — 64 was). **`LoadMemberInt`/`LoadMemberFloat`**
  (the VM analog of the tree-walker's M8 `MemberExpr::eval_int/float`):
  a th==i/f member on a proven non-opt struct base in a local slot
  reads a POD field's scalar straight from the instance's bytes
  (member_keys pool for uid/carets; the boxed-struct/dict/const-member
  residue falls to the shared member_read_core + write_scalar_slot,
  fallback throws stamped with the pooled member caret). With it, 64's
  whole body is typed (load.mem + IntBin/FloatBin, zero boxed ops).
  MEASURED (both changes, full-suite interleaved A/B vs b462642):
  64_struct_create 0.095→0.074s (**0.779x; my/py 0.63x → 0.48x**),
  58_structs 0.875x, suite VM-wall geomean 0.999 (neutral).
  Still open (H1 v2, if ever justified): an inline small-buffer for
  `bytes` (the fresh path's second alloc), the design-level
  inline-POD-in-EvalValue, and global/capture struct bases for
  LoadMember* (local-only today).
- **H2. Dict path — v1 DONE (2026-07-17): two engine-shared micro-fixes
  from the callgrind profile** (bench 62 spent ~1130 instrs per
  `counts[key] += 1`; the roadmap's insert-alloc hypothesis was wrong
  for 62 — it does 18 inserts then 2M UPDATES; the real spread was
  dispatch ~23%, compound-RMW machinery ~15%, the boxed key read ~12%,
  hashtable find ~8%, string eq with memcmp ~6%):
  1. **`LValue::put` inline fast path** (evalvalue.h): the
     overwhelmingly common put is a plain slot write (no container
     back-pointer — frame slots, dict values, globals, captures), yet
     EVERY put paid an out-of-line `get_value_for_put()` call (LTO kept
     it a standalone symbol). The container-less path is now inline;
     the array-element COW path takes the out-of-line `put_slow`.
     Shows up VM-wide (every slot write).
  2. **The string IDENTITY shortcut** (`str_views_eq`, str.cpp.h): two
     views over the same memory range are equal with no memcmp. The hot
     case is a string-keyed dict probe — the stored key is the SAME
     StrObj as the probing value (the key freeze returns strings
     as-is), so every hit compared equal bytes through memcmp.
  MEASURED (full-suite interleaved A/B vs def0580): 62_dict_word_count
  0.096→0.086s (0.896x; my/py 0.47x → 0.42x), broad −4-10% on
  unpack/foreach/dispatch/closures, suite VM-wall geomean **0.992**,
  my/py 4.49-4.50x → **4.57-4.58x** (both runs — the best to date).
  **Still open (H2 v2, DESIGN-LEVEL, needs maintainer sign-off):** a
  flat open-addressing dict (replacing `std::unordered_map` — kills the
  node alloc per insert AND the bucket-chain probe; iteration order may
  change freely, dicts are spec'd unordered), and reserve()
  sizing (no principled size source exists today). 23_dict_insert
  (0.59x, insert-bound: node alloc + rehash growth) is the bench gated
  on it.
- **H3. Strings — v1 DONE (2026-07-17): reserve + borrow in
  join/split.** From bench 31's callgrind profile: `builtin_join` grew
  its result string unreserved (realloc+memcpy per growth step) and
  COPIED each element (a boxed EvalValue + SharedStr refcount round-trip
  per part); `builtin_split` grew its 48-byte-LValue result vector
  unreserved. Fixes (str.cpp.h, engine-shared): join on GENERAL storage
  (a string array is always general) borrows each element by const ref
  and computes the exact result size in a type-checking pre-pass, then
  reserves — the append loop never reallocates (the flat-storage branch
  keeps the old kind-aware loop: elements fail the string check with the
  same TypeError; an empty flat array still joins to ""); split
  pre-counts the pieces (a memchr-driven scan, no stores) and reserves.
  MEASURED (full-suite interleaved A/B vs ec91830): 47_wordcount
  0.034→0.030s (0.882x; my/py 0.51x), 32_str_build_join 0.957x (my/py
  0.72x), 31_str_split_join ~flat (its 1000-piece CSV amortizes vector
  growth well, and the pre-count scan offsets the savings — the
  remaining cost is the inherent per-piece slice LValue + refcount),
  suite VM-wall geomean 0.990, my/py 4.56x → **4.59-4.60x** (both
  runs — the best to date). Deeper string work (a rope/builder for
  `+=` chains, a lazy split) is NOT justified by the numbers — every
  string bench beats CPython (0.00-0.80x).
- **H4. `LValue::get_value_for_put`** (7.4% of append): the per-write
  alias/slice/COW test. A container PROVEN unaliased at codegen (a local
  never copied/sliced — inferencer escape data) could take a pre-checked
  write op. Sound only with a real escape proof; evaluate carefully.

### I. Design-level (only with solid evidence, per the maintainer)

- **I1. Inline POD-struct VALUES in EvalValue.** A POD struct <= 16 bytes
  could live in the 24-byte payload (no heap StructObject, no refcount) —
  struct benches would approach array<int> speed; but it changes
  reference semantics (structs currently alias like arrays) — a REAL
  language-semantics fork. Needs a written semantics proposal first.
- **I2. Open-addressing dict** (replaces std::unordered_map): big wins on
  insert/lookup-heavy code, no third-party deps (hand-rolled, ~300 LoC).
  A data-structure swap, not a language change; still large + risky.
- **I3. NaN-boxing / 16-byte EvalValue.** Halves every slot copy; huge,
  invasive, last resort. Record only.

### J. Measurement hygiene (do alongside)

- Bench wall times at scale 1 are 0.01-0.07s for the loop benches —
  noise swamps 5% effects. Raise --scale (or per-bench iteration counts)
  for the dispatch benches; keep cachegrind Irefs as the tie-breaker.
- Record the CPython version alongside geomeans in the plan (the 3.14
  tail-call interpreter moved the baseline; historical comparisons must
  say which python they ran against).

## Part 4 — top-10 by expected geomean impact

1. **C1 native in-VM call stack — ✅ DONE (2026-07-16), including C2/D.**
   Landed as plans/vm-native-call-stack.md phases A-E: in-VM calls,
   VmInvoker for builtin callbacks, catchable StackOverflowEx, O(1)
   C-stack. Final interleaved full-suite A/B vs 69fef27: PARITY
   (VM-wall 0.999-1.008 across rounds) with recursion 0.68x, sort 0.85x,
   map/make_dict 0.85-0.88x, fib 0.86x. The projected +10-15% geomean did
   NOT materialize: the old do_func_call cost was largely matched by the
   new record/window protocol on tiny bodies, and a ~5% regression had to
   be recovered in four measured steps (the full-suite hard rule dates
   from that recovery). The wins are structural (exceptions walk records,
   zero-copy C-stack, the frame model B1/E-class work needs) plus the
   call-bench improvements above. Zero-copy binding (Phase E) was
   MEASURED AND DECLINED (bind = ~1.6% of the most call-bound bench).
2. **A1 computed-goto dispatch (+A2 hot/cold split, A3 ordering)** —
   recovers the measured 10-30% regression on 26 dispatch-bound benches
   and lifts everything else. Est. +8-15% broad.
3. **C2 callback frame reuse in map/filter/sort/make_dict** — the cheap
   80% of C1 for the higher-order benches specifically; fixes the four
   "VM slower than TW" rows. Est. +3-5% now, keeps value under C1.
4. **B1+B2 opcode specialization — ✅ DONE (2026-07-16, commit aa372a8).**
   23 per-op/per-shape variants selected by the in-place post-codegen
   specialize_arith_ops pass. Measured (interleaved full-suite A/B):
   VM-wall geomean **-6.3%**, suite 4.09-4.17x -> **4.45-4.48x vs
   CPython** - past the pre-C1 peak; 01_while -25%, 03_int -20%,
   mandelbrot -19%, bit benches -18%, broad collateral wins. Exceeded
   the +3-6% estimate (the inner switch + decode branches were worth
   more under computed-goto than projected).
5. **D1 AppendV (+D2 arg windows)** — ~23% marshaling tax on append-
   heavy code (13, wordcount, matrix, sieve builders). Est. +2-4%.
6. **E1-E4 post-codegen peephole pass** — move/dead-temp elimination,
   jump threading, systematic fusion; also shrinks frames (E2 helps
   every call). Est. +2-5% and a permanent framework.
7. **F1 typed builtin/member reads into arith** — unblocks
   40_math_builtins-class code (boxed round-trips with zero fallbacks).
   Est. +2-3%.
8. **G1 exception cross-frame on the VM stack** — kills the last
   CPython LOSS (69 at 1.88); small geomean (~2%) but closes the
   "slower than Python" list and completes the vm-endgame goal.
9. **B3 Instr shrink to <=32B** — bytecode D-cache halves; compounds
   with A1/B1. Est. +1-3%; do once the opcode set stabilizes.
10. **H1-H3 runtime allocs (struct pool, dict insert, string builders)**
    — engine-neutral C++ wins that cap the remaining laggards (63/64/
    77 struct, 47/62 dict, 31/32 strings). Est. +2-4% combined.

Realistic compounding: (1)+(2) alone ≈ x0.78-0.85 on the geomean ratio —
i.e. 3.9x -> ~4.7-5.0x; items 3-10 provide the margin and the tail. The
order of execution should be 2 -> 3 -> 4 -> 5 -> 6 (all contained, each
independently gated by `run.py --vm --baseline`) while 1 (the call stack)
is designed properly in its own plan; 1 then subsumes/absorbs 3 and
enables 8.

## Part 5 — ground rules for this work

- **FULL-SUITE ONLY (hard rule, 2026-07-16):** perf claims come from full
  bench/run.py runs of both binaries, same session, interleaved and
  repeated; three-digit geomeans. A subset probe or a cross-session my/py
  comparison masked a real ~5% suite regression during C1 Phase C - the
  maintainer's controlled re-run (multiple full runs per commit) caught it.

- Every step: differential `-rt` green + `run.py --vm --baseline` gate;
  cachegrind Irefs on 01/44/60 + one callback bench recorded in the
  commit message (wall clock on WSL2 is too noisy for 3% decisions).
- Don't perturb dispatch-loop layout blind (the recorded 12.5% front-end
  lesson): land A1 FIRST, then re-baseline, then judge per-op changes.
- No lazy anything; no third-party deps; MSVC keeps the switch dispatch.
