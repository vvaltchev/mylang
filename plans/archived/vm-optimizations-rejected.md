# VM optimizations — rejected and closed (archived record)

Not in flight: these are ideas that were TRIED AND REJECTED, plus the
ledger of parked entries that have since been merged. Split out of
`plans/vm-optimizations-deferred.md`, which now holds only OPEN items.

**This file is the reason not to re-attempt these.** Several were fully
built and proven correct before being reverted on measurement - the cost
of re-discovering that is exactly what the entries below prevent. Anyone
about to propose one of these should read its measurement first.

## Rejected (do not revisit without new evidence)

- **Splitting the cold handlers out of `vm_run_chunk`** (measured,
  carried forward from `plans/archived/vm-ast-free.md`, now archived): shrinking the
  dispatch function 6952 -> 5803 instructions made the front-end
  regression WORSE, not better (geomean 1.168 vs 1.125). The "smaller
  dispatch text is faster" intuition is not reliable here - this is the
  same loop-body TEXT effect that later cost +3.6M Ir when the catch
  dispatch was inlined into `vm_dispatch`.
- **Converting the dispatch switch to a `.rodata` jump table**
  (measured, same source): ~2-3% SLOWER than the compiler's own switch
  lowering. Computed-goto (`CGOTO`) is the form that did pay.

- **The two-tier 16-byte instruction (B3 stage 3) — BUILT, MEASURED,
  REVERTED (2026-07-18).** The full design was implemented and proven
  CORRECT (1541 -rt + differential + 400-program fuzz + six configs
  green): `PInstr` 16B (int16 targets, int32 payloads, dual16 halves,
  narrow FLOAT32 lits when losslessly representable), a 16-byte
  EXTENSION WORD carrying 64-bit payloads (op-byte = an ExtWord canary,
  consumed by the owning handler - never dispatched, no flag in the
  main loop, position-implicit), a()/b() resolving wide transparently,
  narrow gates on every direct-reader op, and a late `pack_chunk` (the
  peephole/fusions kept working on the fat 32B form). MEASURED
  (full-suite interleaved A/B vs 354a770): **VM-wall geomean 1.122
  (+12.2%), my/py 4.89x → 4.44x** - the dispatch-bound tier regressed
  30-78% (04_float_arith 1.78x, 59_bit_hash 1.73x, 61_popcount 1.50x,
  sieves 1.33-1.41x), INCLUDING benches whose handlers were untouched
  (the specialized bit ops) - i.e. the loss is decode ALU + the
  whole-dispatch-loop code-layout perturbation, and the density bought
  nothing back (hot loops are L1-resident; B3's 56→32 step had already
  captured the real win). This is the local confirmation of the
  literature (CPython wordcode variable→fixed for speed; Lua/LuaJIT
  fixed 32-bit; Shi/Gregg dispatch count >> byte density). Bytecode
  DENSITY is a WIRE-format concern: apply compact encoding to the
  `.myv` FILE, decode to the fixed 32-byte Instr at load.

- **Flattening the catch handler table (#81) — BUILT, MEASURED
  NEGATIVE, DISCARDED (2026-08-12).** Matching a catch clause walks
  `handler_sites` → the site's `vector<HandlerClause>` → the clause's
  `vector<const UniqueId *>`, ~32 Ir/throw; the idea was one flat pool
  per chunk with a (base, count) per site. Measured against `f4edb53`
  (callgrind Ir): 42_exceptions **+1.14%**, 70_exc_runtime_error
  **+2.88%** (both at `OPT=1 ASSERTS=0`, jit on); 69 and 72 flat. Note
  the ASSERTS trap — the same change read only +0.22% / +0.55% at
  ASSERTS=1, 5x milder, because the NESTED baseline was the side paying
  the hardened container access.
  **Why the premise was wrong, which is the part worth keeping:** the
  inner vector's `_M_start`/`_M_finish` live INSIDE the site struct the
  dispatch has already loaded, so the nested range-for is a plain
  pointer loop with NO extra dependent load, while the flat form must
  additionally load `handler_clauses.data()` (a different cache line of
  `Chunk`), `clause_base` and `n_clauses`. "Nested vectors are three
  pointer hops" is true of the TYPE and false of the generated CODE
  when the outer element is already in hand.
  Two mechanical lessons from three rounds of tuning (the first cut was
  +1.76%/+3.96%): `_GLIBCXX_ASSERTIONS` bounds-checks every
  `vector::operator[]`, so indexing the flat pool put a `size()`
  compare on EVERY element of the matcher scan where the nested form
  checked once and iterated unchecked (**+28 Ir/throw** — a flattening
  only pays if the hot scan goes through `.data()`); and a 12-byte
  element makes `cls[ci]` cost an `imul` per access where the range-for
  was a pointer increment (~0.8% on 42).
  Parked green but unmerged on `wip/81-flatten-handler-table`
  (`8486a37`).
- **Flat open-addressing dict** — REJECTED by the maintainer
  (node-pointer stability); the approved form was the H2 v2
  `unordered_map` pool allocator (`poolalloc.h`), which shipped.
- **BinOpV→CompoundV fusion** — two throw sources with different carets
  cannot share one pc's loc entry (the E4 class rule).
- **Zero-copy arg binding** (native-call-stack Phase E) — measured and
  declined: the bind is ~2 instructions; the protocol around it is
  what costs.
- **EvalValue assign-operator surgery** (re-profile #3) — tried
  THREE times, declined three times: the operators inline at hundreds
  of sites, ANY textual growth perturbs the whole binary. The second
  attempt was `value-model-campaign.md`'s Tier 2a, whose failure mode
  is FUNDAMENTAL, not a tuning miss: either inline bloat pushes the
  hot same-type path out-of-line everywhere, or moving the type-change
  arm out-of-line regresses type-change-heavy code. The third was H5
  (2026-08-12, devirtualizing `operator=`), which made
  `76_funcval_dispatch` WORSE twice and was reverted.
  **The RULE this earns** (from `archived/value-model-campaign.md`):
  value-model micro-opts on hot INLINE functions (`operator=`,
  `boxed_operand`) are codegen-sensitive and rarely a clean win —
  prefer removing whole ops / whole copies over machinery tweaks.
  Note the still-OPEN sibling in the deferred file (a native-immediate
  path in `vm_num_binop`) is interesting precisely because it might
  capture the same win WITHOUT touching `boxed_operand`'s inlining.
- **SIMD / vectorization** — explicitly DEFERRED with reasoning
  (`archived/native-gap-roadmap.md` lever 6), recorded here so the
  decision is not re-litigated from scratch:
  `30_str_index_iterate` (43x), `46_matrix` (35x) and the sieve/bit
  benches are vectorized in C++ — 2-16 lanes per instruction, and
  scalar native code tops out ~2-4x behind that. A large separate
  project, and NOT needed to reach the geomean target, so deferred by
  choice rather than left undone.
- **Slot-level slice borrowing** (`archived/native-gap-roadmap.md`,
  lever 3 option (b)) — explicitly NOT pursued: the residual prize no
  longer justifies the value-model cliffs it would open.
- **The emitted `cur_seg >= 0` guard** (`archived/cpp-gap-extremes.md`)
  — 2 instructions, provably true whenever a fragment is running, and
  KEPT anyway: it guards a raw `[segs + cur_seg*8]` load, and the
  project's defensive-check preference outweighs 1.4% on one probe.
  Recorded as **overturnable** — unlike the rest of this file, this one
  may be revisited without new evidence if the policy changes.
- **Node-level superinstructions on the TREE-WALKER** — killed by the
  cachegrind study (~1 instr/iter). The VM-side analogue is alive as
  the E4 peephole fusion framework, gated per-fusion by wall clock.

## Closed ledger (merged / superseded — the original entries)

- **The residual fusion batch (roadmap #9)** → SHIPPED 2026-07-17
  (IntAddStep / ForStepElemInt / StructFieldAddInt + the
  `visit_use_def` barrier fix for the struct/member loads; suite
  VM-wall 0.987, 65_struct_field_sum 0.783x, my/py → 4.89-4.93x).

- **C2 computed-goto dispatch** → SHIPPED as `CGOTO` (default 1;
  ~10% geomean on dispatch-bound loops, −25-42% indirect-branch
  mispredicts). MSVC keeps the switch, as planned.
- **C4 `ModConst`** → SUPERSEDED by B1/B2's `IntModRI` (nonzero-imm
  mod, part of the 23-variant specialized-arith batch).
- **C1 typed reads** → DONE (DictLoad*/LoadStructField*/LoadElem* +
  H1's LoadMemberInt/Float); its residual (a boxed builtin result
  feeding typed arith, the 40_math_builtins gate) → DONE as F1
  `MathFnV`.
- **C5 bool-compare re-boxing** → DONE (JumpUnless{Int,Float}Cmp,
  JumpUnlessTrueV, the typed-ternary lowering; `JumpIfFalse` itself is
  deleted).
- **`Instr` shrink** → DONE as B3 (56→32 bytes, static_asserted; the
  node handle is codegen-only `CgInstr`).
- **The "parked while fallbacks remain" premise** → obsolete: the
  no-fail codegen deleted every fallback op; zero-AST is machine-proven
  (`vm_ast_teardown`).
