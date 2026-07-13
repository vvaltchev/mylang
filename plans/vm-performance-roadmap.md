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
- **B3. Shrink `Instr` 56 -> <=32 bytes.** Operand is 16 bytes x2; pack
  to {i32 slot-or-imm + 2 flag bits}; move the RARE wide fields (float
  immediates, big literals) to the const pool. Halves the bytecode
  D-cache footprint and speeds decode. (Do AFTER A1/B1/B2 — encoding
  churn is cheapest once the opcode set settles.)
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

- **D1. Append/push fast op.** `vm_call_builtin_lv_rest` is ~23% of
  13_array_append. `append(a, x)` deserves a dedicated `AppendV` op:
  arg0's LValue* formed from the slot (as today) and the VALUE taken
  straight from its register — no rest-run copy, no ArgLocs deref, no
  builtin function-pointer call. (append/pop/len are the dict/array
  workhorses; len already folds into ArrLen where proven.)
- **D2. Pass builtin value-args as a slot WINDOW, not copies.**
  `CallBuiltinV` copies each arg EvalValue into a buffer; a `const
  LValue *args + n` view over the frame run (exactly VmArgs) removes a
  refcount bump + 32-byte copy per arg. Needs the func_v ABI widened to
  the view type — mechanical but touches every migrated builtin.

### E. A post-codegen optimizer (the "LLVM pass" the maintainer invited)

Run over the finished chunk, before locs extraction:

- **E1. Copy propagation / MoveV elimination.** The codegen already
  retargets ad-hoc; a systematic pass catches the rest (ternary arms,
  coalesce, arg staging into runs).
- **E2. Dead-temp elimination + temp REUSE (linear-scan over temps).**
  Shrinks `n_temps`, so every call's `Frame::init` constructs fewer
  48-byte LValues — recursion pays Frame::init per call.
- **E3. Jump threading / fallthrough cleanup** (jump-to-jump, jump-to-
  next, branch-over-jump inversion).
- **E4. The fusion pass** — pattern-match adjacent ops into the B4
  superinstructions instead of hand-coding each shape in the codegen (one
  place to add patterns, codegen stays simple).

### F. Widen the typed (unboxed) tier

- **F1. Typed member/subscript/builtin-result reads feeding arith**
  (the old Part C1): `bin.v` chains where one operand is a `member.v` /
  `call.blt.v` result box+unbox per element. 40_math_builtins (.42, zero
  fallbacks) is gated on exactly this. Typed variants: `CallBuiltinIntV`
  (sqrt/abs/len -> int/float dst), `MemberIntV` beyond dicts.
- **F2. Bool-typed conditions.** `while (flag)` boxes through
  JumpUnlessTrueV; a `JumpUnlessBool` on a proven-bool slot reads the
  byte directly.

### G. Exceptions endgame

- **G1. Cross-frame propagation without ANY landing pad** — rides C1: an
  in-VM frame stack makes an unhandled exception a WALK of the VM stack
  (pop frames, check handler tables) with zero C++ unwinding. Today each
  crossed frame converts throw->signal at ONE boundary but still enters
  do_func_call's machinery per frame. 69_exc_crossframe (my/py 1.88, the
  only real CPython LOSS left) is the target; CPython's zero-cost-try /
  cheap-raise is the bar.

### H. Runtime / library-level (engine-neutral; caps several laggards)

- **H1. Struct creation** (64_struct_create 0.61, 77 0.65): per-instance
  `StructObject` heap alloc + intrusive count. Options: a small-object
  pool for StructObjects; construct-in-place into locals; and (design-
  level, below) inline POD structs in EvalValue.
- **H2. Dict insert path** (47/62 wordcount 0.49-0.61, 26_dict_insert):
  `unordered_map<EvalValue,LValue>` node alloc per insert + hash dispatch
  via Type vtable. Consider reserve() from ArrHint-style size hints, and
  a flat open-addressing map (design-level).
- **H3. Strings** (31_split_join 0.83, 32_build_join 0.66, 28_concat):
  split() building per-token SharedStr allocations; join with a single
  size-precomputed buffer (partially done); `+=` chains via a rope/
  builder. These are builtin-body C++ optimizations, same on both
  engines.
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

1. **C1 native in-VM call stack** — ~30% of callback benches is
   do_func_call; doubles the call-bound dozen (09/10/11/12/34/35/63/64/
   67/76 + samples). Est. geomean +10-15%. Big project; stage it.
2. **A1 computed-goto dispatch (+A2 hot/cold split, A3 ordering)** —
   recovers the measured 10-30% regression on 26 dispatch-bound benches
   and lifts everything else. Est. +8-15% broad.
3. **C2 callback frame reuse in map/filter/sort/make_dict** — the cheap
   80% of C1 for the higher-order benches specifically; fixes the four
   "VM slower than TW" rows. Est. +3-5% now, keeps value under C1.
4. **B1+B2 opcode specialization (per-op arith, reg/imm variants,
   ModConst)** — removes a second switch + 2 decode branches from THE
   hottest ops. Est. +3-6% broad, synergistic with A1.
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

- Every step: differential `-rt` green + `run.py --vm --baseline` gate;
  cachegrind Irefs on 01/44/60 + one callback bench recorded in the
  commit message (wall clock on WSL2 is too noisy for 3% decisions).
- Don't perturb dispatch-loop layout blind (the recorded 12.5% front-end
  lesson): land A1 FIRST, then re-baseline, then judge per-op changes.
- No lazy anything; no third-party deps; MSVC keeps the switch dispatch.
