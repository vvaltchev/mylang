# VM optimizations — deferred (parking lot)

The CURRENT backlog of perf ideas that are real but not scheduled. The
live, evidence-ordered work list is the RE-PROFILE top-10 in
`plans/vm-performance-roadmap.md`; this file holds only what is parked.
(2026-07-17 cleanup: most of the original entries here have since been
MERGED — see the closed ledger at the bottom.)

Ground rules (maintainer): a change must be perf-neutral-or-better in
the worst case, verified by the FULL-SUITE interleaved A/B rule
(CLAUDE.md, Benchmarks). Correctness is gated by the `-rt` VM
differential + `tests/nested_fuzz.py`.

---

## Open

- **Per-chunk "possibly-reference slots" list** (roadmap #1's residual):
  `vm_leave_call`'s O(nslots) reset scan could skip all-scalar frames
  (the fib-class) if codegen recorded which slots can ever hold a
  non-trivial value.
- **E2 — peephole temp renumbering** (`plans/vm-peephole.md`):
  evaluated + deferred — the native call stack made per-call temp cost
  ~nil, so compacting `n_temps` buys little. Revisit only if a profile
  shows frame-size cost.
- **H1 v2 — inline small-buffer for BOXED structs** (roadmap, alloc
  study): a boxed instance's `vector<LValue>` fields could live inline
  under N slots. Only if a boxed-struct bench ever matters.
- **Value-template v2 — REPL cross-input value instantiation**
  (`plans/value-template-instantiation.md` known gap): an input-1 array
  called indirectly from input 2 keeps the boxed base (correct,
  unoptimized; pinned by a `repl:` test).
- **C3 residual — builtin arg-view ABI**: pass the frame run to
  `func_v` by view instead of copying into the stack `EvalValue[8]`.
  Marginal (scalar args copy cheap; only non-trivial args pay a
  refcount bump) and touches all ~84 signatures; its valuable half
  (AST-free carets) already shipped as `ArgLocs`/`builtin_calls`.
- **Machine-code JIT (x86-64)** — now DESIGNED as the incremental
  baseline-AOT tier: `plans/native-aot.md` (2026-07-18; EnterNative +
  per-chunk RX side buffer, run-granularity fragments over the audited
  scalar tier, bail-to-interpreter, never-throws rule, phases N0-N5).
  Runs on-the-fly post-codegen/post-`.myv`-load; never serialized.
- **A PMU box.** WSL2 has no hardware counters, so front-end/layout
  effects (the dominant residual on dispatch-bound loops — see
  `[[vm-dispatch-frontend-regression]]`) only show as wall-clock deltas
  whose cause can't be confirmed. Any further layout-sensitive work
  wants `perf stat` on bare metal.

## Rejected (do not revisit without new evidence)

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
