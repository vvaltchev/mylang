# Typed-invariant arrays: the C staircase (toward N7)

Status: **PLANNED, not started.** Written 2026-08-02, after the element-
tier matrix (#92-#95) completed and the maintainer picked (C) as the
destination, with the explicit constraint that it lands as SMALL
INCREMENTAL STEPS - big changes are harder to land correctly.

## What C is

The measured residual on the array-access class (plans/unboxing.md, the
91.5 Ir/inner-iteration decomposition of 46_matrix_mult) has two
structural components left:

1. **Invariant re-verification.** Every element access re-runs the
   base's type/slice/kind guards and re-derives its data pointer and
   count - per ELEMENT, for a base that provably cannot change inside
   the loop. C++ proves these facts once per program.
2. **Temp traffic between ops** - producer writes a frame slot, the
   consumer immediately reloads it.

C is the removal of (1) as ARCHITECTURE rather than per-op patches:
stop re-deriving at runtime what the compiler already proved. Its
endgame is the N7 arc - typed, unboxed locals - but it is reached by
steps that are each independently sound, landable, and measurable.

## Where A and B fit (the question this file answers)

- **A (adjacent dead-temp forwarding) is NOT part of C.** It removes
  component (2) - a register-allocation concern that survives even with
  perfect type trust. It is small, independent, and lands FIRST (its
  own work, not tracked here; see plans/unboxing.md's lever list).
- **B (loop-preheader guard hoisting) IS C's first step - C1 below.**
  What B hoists (base -> shobj -> data/count, guards checked once per
  loop) is exactly the re-derivation C wants gone. B proves it by
  DATAFLOW INVARIANCE (machinery LICM already has); C's later steps
  prove it by TYPE TRUST (which needs inference airtight). Doing B
  standalone and then C would build the same thing twice - so B is
  folded in as the opening increment, not a separate project.

## The staircase

Each step stands alone: green suite in all 5 modes, execution-proven
by an emitted-code counter, sabotage-verified guards, one callgrind
A/B at `OPT=1 ASSERTS=0`. No step requires the next.

### C1 - per-loop guard + navigation hoisting (~ the old B)

Hoist an invariant array base's navigation OUT of the loop body: at the
loop preheader (before the back-edge target), verify type/non-slice/
kind ONCE and load data pointer + count into callee-saved registers;
the per-element code indexes off the registers with only the bounds
check left. On guard failure at the preheader, fall to the current
per-element code for the WHOLE loop (approach A - a compile-time
decline path, never a mid-loop bail with stale registers).

- Soundness = INVARIANCE, proven the way LICM already proves it:
  `fr_immutable` / `mut_len` / `mut_content` - the base's slot is not
  reassigned in the loop and its content/length not mutated (an element
  WRITE to the base disqualifies: COW detach would move the data
  pointer). The pure-callback rules (`callable_arg_mask`) carry over
  unchanged.
- Register budget: the N5 pool is r12-r15 and `pick_cached_slots`
  already arbitrates it; a hoisted base costs 2 registers (data,
  count). Start with ONE hoisted base per loop (the hottest by use
  count) so the int-counter pins keep their slots; widen later.
- The bounds check stays per element (indices genuinely vary). What
  disappears per element: the type-tag load+cmp, the slice-flag cmp,
  the kind cmp, the shobj load, the data/finish loads and the count
  subtraction - the measured ~10-15 of 91.5, more on nested shapes.
- GOTCHA recorded up front: a helper call inside the loop preserves
  r12-r15 (callee-saved) but a PREP/slow-tier path that CLONES the
  base (COW) moves the data pointer - any loop containing a WRITE to
  the hoisted base must not hoist it (the mut_content gate above
  already excludes this; the sabotage test is a store-through-alias
  shape).

### C2 - widen the register cache beyond int slots

`pick_cached_slots` pins only int SCALAR locals today. Extend the pool
model to (a) float slots (xmm registers are all caller-saved, so this
interacts with helper calls - may need the C1 spill discipline), and
(b) the C1-hoisted pointers as first-class pool citizens with the same
`bad()` disqualification rules. This is the allocator groundwork the
register plan (plans/jit-registers.md) names as next; A's forwarding
machinery feeds it (a forwarded value is a register-resident value with
a one-op lifetime - the allocator generalizes the lifetime).

### C3 - typed frame slots for proven scalars

Narrow what a slot write must do when inference proved the slot's type:
the type-word store (half of every scalar two-store) disappears for
slots that can never hold anything else, and `ref_slots` narrows to
slots that can genuinely hold references (today an inferred-int param
still sits in the release-scan list). This is the step the return-path
notes (plans/cpp-gap-extremes.md) flagged as a DELIBERATE soundness
decision: it trades a memory-lifetime guarantee for inference being
airtight, and the failure mode is a silently retained reference or a
garbage type pointer. Prerequisites, in order:
- a VM_HARDENING audit mode that re-verifies every "provably typed"
  slot's tag on every write (the audit-net pattern ref_slots already
  uses), running green across the differential + fuzzer for a while
  BEFORE any code relies on the proof;
- the #96 class of gap (an engine path wider than the checker assumed)
  is exactly what this step must not have - every widen/coerce site
  enumerated first.

### C4 - unboxed typed locals end to end (N7)

The endgame: a proven-int local IS a machine word (no 48-byte LValue
round trip), a proven-flat-array base IS a (pointer, count) pair kept
in registers across its live range, boxing only at genuine dyn/ref
boundaries. C1-C3 each remove a slice of this cost while keeping the
value model intact; C4 changes the model. Do NOT scope C4 in detail
now - re-measure after C1-C3 and scope it against real numbers (the
same discipline unboxing.md applied to its option B).

## Non-goals and discipline

- The SUITE geomean is not the gate for C1 (one hot corpus program has
  the nested shape); the per-iteration Ir on the probe loops and
  46_matrix_mult is. Say so in every measurement report.
- No step starts before the previous is committed and green - and no
  step is a prerequisite-in-hiding for the next: each must be
  revertable alone.
- The engine differential CANNOT see these changes (emitter-level);
  every step needs the JIT-on-vs-off corpus differential, the fuzzer,
  and per-guard sabotage runs - the #92-#95 proof pattern.
- CLAUDE.md's vacuous-test trap list applies in full; C1's tests need
  shapes where LICM does NOT already hoist the row (outer index varying
  with the inner loop) and bases that defeat const-arg specialization.
