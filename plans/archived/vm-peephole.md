# The post-codegen peephole pass (roadmap E1-E4)

Status: COMPLETE. Inc 1 (infra + E3) + Inc 2 (E1 MoveV elimination)
DONE 2026-07-16; E2 evaluated + deferred; the typed-ternary follow-up
and the first TWO E4 fusions (IntAddModRI, JumpUnlessElemInt) DONE
2026-07-17 — see the Follow-ups section below and the roadmap E entry.
Remaining fusion candidates live in the roadmap's 2026-07-17 top-10
(item 9).

Static results (bench/ + samples/): total instrs 3761 → 3587 (−4.6%),
MoveVs 399 → 275 (−31%), fib$0's chunk 68 → 56 instrs (−18% — every
ternary-arm `producer; move; jmp` collapsed to `producer-into-dst; jmp`).

## What this is

The systematic post-codegen optimizer over a finished `Chunk` — the
"LLVM pass" the maintainer invited: instruction DELETION with pc
remapping, jump threading done right (E-v1's standalone retargeting was
measured a decline — see roadmap E3 — because the dead Jumps stayed),
copy propagation over the `MoveV`s the codegen's ad-hoc retargeting
misses, and dead-temp shrinking so every call constructs a smaller
frame. Compile-time only: ZERO new runtime ops, so `vm_run_chunk`'s text
is untouched (the front-end/layout hazard is limited to LTO-level
shifts).

## Placement (the ordering that avoids side-table remapping)

`codegen_chunk` tail: **emit → PEEPHOLE → extract_locs →
specialize_arith_ops → verify_ast_free**.

Running BEFORE `extract_locs` is the load-bearing choice: the loc side
table, `inline_ctxs`, and the `node_idx` nulling all happen on the
ALREADY-COMPACTED code, so the pass never remaps a side table — only
`Instr` pc fields. (`Instr::node_idx` handles are indices into the
codegen-transient `ast_nodes`, carried inside the moved `Instr` structs,
so deletion shifts them for free; a deleted op's entry is simply never
read.) `specialize_arith_ops` stays after `extract_locs` and changes no
pcs. All pools (`consts`, `builtin_calls`, `throws`, …) are indexed by
Instr OPERANDS, never by pc — untouched by deletion.

## The pc-field table (the E-v1 trap, centralized)

`visit_pc_fields(Instr&, F)` is THE single audited enumeration of every
Instr field that holds a pc — used by remapping (mandatory), threading,
and the CFG successor walk. Audited 2026-07-16 by grepping every
`pc = in->...` in vm.cpp:

| op                  | pc field  | NOT a pc                       |
|---------------------|-----------|--------------------------------|
| Jump                | target    |                                |
| JumpUnlessIntCmp    | target    | a, b = operands                |
| JumpUnlessFloatCmp  | target    | a, b = operands                |
| JumpUnlessTrueV     | target    | target2 = the value SLOT       |
| JumpIfNotNoneV      | target    | a = the value operand          |
| ForLoopStep         | target    | **target2 = the COUNTER SLOT** |
| DictIterNext        | target    | a/b = binds, iter id           |
| ForeachDynNext      | target    | shape/targets operands         |
| CatchTest           | target    | a.lit = catch_types idx        |
| PushHandler         | target    | (feeds the runtime handler)    |

`SetPend::target` is a **Pend enum value**, not a pc. No other op reads
a pc from a field (verified against every `pc =` site in vm.cpp). A new
branching op MUST be added here or `ml_peephole` static checks/tests
will not save you — the fuzzer will (13/300 caught ForLoopStep::target2
in E-v1); ALWAYS run nested_fuzz.py after touching this pass.

## CFG + successors

Successor edges for reachability and liveness: every op falls through
(`pc+1`) except `Jump` (target only), `ReturnV`/`Halt` (none),
`Throw`/`Reraise`/`Rethrow` (none — control resumes only at handler
pcs, which are edges from their `PushHandler.target`). Branch ops have
`{target, pc+1}`. `EndFinally`'s reraise path dispatches to a handler
pc (a PushHandler edge) or C++-rethrows; its normal path falls through.

## Increments

- **Inc 1 — infra + E3 (jump cleanup).** `visit_pc_fields`, reach DFS,
  `compact_chunk` (drop marked ops, build old→new pc map, rewrite pc
  fields). Rules: thread jump chains (the E-v1 logic, now the skipped
  Jumps actually DIE), delete jump-to-next, delete unreachable ops,
  invert an INT branch-over-jump (`i.jmp.ifnot C L1; jmp L2; L1:` →
  `i.jmp.ifnot !C L2`). Float compare inversion is EXCLUDED — NaN:
  `!(a<b)` is not `a>=b`. JumpUnlessTrueV has no inverted form — skip.
- **Inc 2 — E1 (copy prop / MoveV elimination).** Backward liveness
  over TEMP slots only (bit-set per pc, fixpoint over the CFG). The
  generalized rule handles BOTH move shapes: for a `MoveV d=tX` (tX a
  temp, dead after the move), EVERY predecessor must be a retargetable
  producer of tX — the fall-through one directly (the ARM-move shape,
  `prod; move; jmp`, fib's), and each branch into the move must be a
  plain `Jump` (a conditional entering the join disqualifies) whose own
  fall-through predecessor is such a producer and which nothing else
  enters (the JOIN-move shape: both ternary arms produce tX, one move
  at the join — all producers retarget to d). Then delete the move
  (neutralized to a jump-to-next; the same round's deletion removes
  it). Dead-STORE deletion (side-effect-free whitelist writing a dead
  temp) remains a future rule.
- **Inc 3 — E2 (temp shrink): EVALUATED + DEFERRED.** The native call
  stack already made the per-call temp cost ~nil: an in-VM window push
  does NOT construct slots (segments are constructed once; pop resets
  only REFERENCE slots by scanning content, independent of n_temps), so
  shrinking n_temps saves only segment stack SPACE. Boundary calls
  (main, builtin callbacks) do construct `frame_size` LValues, but they
  are rare. Full dense renumbering needs the complete slot-field
  visitor (every op) for a ~nil measured win — not worth the risk
  today. Revisit if a profile ever shows window space or boundary
  Frame::init.
- **E4 (fusion framework → fusions).** The pass IS the framework — a
  fusion rule is another match/rewrite over the instruction window.
  The first two fusions SHIPPED 2026-07-17 (see Follow-ups below);
  B1/B2 stays a separate in-place rewrite.

## Quantified expectations (measured before building)

fib$0's 68-instr chunk: ~6 MoveV+Jump pairs from ternary arms, ~9-12
MoveVs per chunk across recursion/matrix benches. The BULK of fib's
body is `bin.v`/`cmp.v` (BOXED — the ternary-arm/condition lowering
boxes even int-proven operands: a TYPED TERNARY lowering in
compile_int/float_expr is the bigger fib win, recorded as an F-class
follow-up, NOT this pass). So the peephole's expected effect is a
MODEST wall win + smaller chunks/frames; the hard rule applies — a
full-suite interleaved A/B decides, and a null result with clean
infrastructure is an acceptable outcome (the infra is what E4 fusions
and the typed-ternary rewrite build on).

## Post-land lessons (the red-CI fix, 2026-07-17)

1. **Op-count tests are COUPLED to this pass** — a peephole improvement
   legitimately BREAKS stale pins. The first landing turned all 4 CI
   lanes red on two pins the pass had IMPROVED: `break`/`continue`
   jumps now INVERT away (`if(i==3) continue` → one
   `i.jmp.ifnot i != 3 -> for.step`, zero plain Jumps), and an
   always-returning try body's normal fall-out (`try.pop; set.pend
   normal; jmp`) is unreachable (the return inlines its finally) and is
   deleted — `setpend` drops 2→1. When a codegen-shape test fails after
   a peephole change, DUMP the shape and decide improved-vs-broken;
   re-pin only after reading the bytecode.
2. **Read the `-rt` HEADLINE (`Tests passed: N/M`), never a bare tail**
   — the differential line prints [PASS] independently AFTER a failing
   headline, and `tail -3` showed exactly the wrong two lines. The
   red push happened because of this misread; grep the headline.

## Follow-ups recorded

- TYPED TERNARY VALUE lowering — **DONE (2026-07-17)**, two layers:
  1. **The M8 walker gap (both engines):** `specialize_children`
     (inferencer.cpp) had NO TernaryExpr/CoalesceExpr case, so a
     ternary's cond/arms were NEVER M8-specialized — the tree-walker
     ran them boxed too (the recursion-unroll's guard ternaries, i.e.
     fib's whole body, were the visible cost). Fixed: both now recurse.
  2. **`try_typed_ternary` (codegen.cpp):** a th==i/f TernaryExpr in
     the typed compilers emits a native JumpUnless{Int,Float}Cmp to the
     else arm (a non-compare condition boxes to JumpUnlessTrueV, arms
     still typed) and both arms produce into a common dst via
     MoveV/LoadImm — which THIS pass's E1 join-move rule then retargets
     away. fib$0's chunk: 68 (pre-peephole) → 56 (peephole) → **50,
     fully native** (i.bin/i.jmp.ifnot/call.cached, zero boxed ops,
     zero moves). MEASURED (full-suite interleaved A/B vs 28108d2):
     09_fib_recursive 0.006→0.004s (**0.67x**), suite VM-wall geomean
     0.990, broad −3-9% on arith/call benches; my/py 4.46-4.47x.
- E4 fusions — **DONE (2026-07-17), profile-driven.** A scratch op-pair
  profiler (CGOTO=0 build, reverted after use) counted 760M executed
  adjacent-pair dispatches over the bench suite; the distribution is
  FLAT (top pairs 2-5%, half of them unfusible control shapes). Two
  fusions shipped, chosen by coverage-per-op-added AND caret safety:
  1. **IntAddModRI** (`IntBin(+) t = a+b; IntBin(%) dst = t%IMM` →
     `dst = (a+b)%IMM`, 3.4% of dispatches — the checksum shape).
     Never throws (imm nonzero + int32-gated; the add wraps) —
     loc/node-free.
  2. **JumpUnlessElemInt** (`LoadElemInt t = arr[i];
     JumpUnlessTrueV t, L` → one load+test+branch, 1.7% — the sieve
     test). Keeps the LOAD's node in place, so the OOB caret is
     byte-identical; the temp must be dead on BOTH successor paths.
  Both rules run in the liveness block (temp-dead + no-branch-target
  conditions; conservative against same-round mutations). **REJECTED:
  `BinOpV→CompoundV` (2.9%)** — two distinct throw sources with
  different carets cannot share one pc's loc entry; fusing it would
  break byte-identical error reporting. MEASURED (full-suite
  interleaved A/B vs 0e8266f): 68_nested 0.957x, 60_bit_sieve 0.958x,
  broad −3-4%, suite VM-wall geomean **0.981**, my/py 4.70-4.71x →
  **4.75x** (both runs).
