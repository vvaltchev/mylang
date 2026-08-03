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

- **Flat open-addressing dict** — REJECTED by the maintainer
  (node-pointer stability); the approved form was the H2 v2
  `unordered_map` pool allocator (`poolalloc.h`), which shipped.
- **BinOpV→CompoundV fusion** — two throw sources with different carets
  cannot share one pc's loc entry (the E4 class rule).
- **Zero-copy arg binding** (native-call-stack Phase E) — measured and
  declined: the bind is ~2 instructions; the protocol around it is
  what costs.
- **EvalValue assign-operator surgery** (re-profile #3) — tried twice,
  declined twice: the operators inline at hundreds of sites, ANY
  textual growth perturbs the whole binary.
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
