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

**LANDED 2026-08-03**, after three designs, each decided by a
measurement that contradicted the sketch here - the record matters more
than the sketch did:

1. **Run-scoped, callee-saved regs** (this section's original plan):
   whitelist over the RUN, nav at fragment entry, 2 regs reserved from
   the N5 pool. Fired on ZERO benches - since the #56 call-deletability
   work a run spans the whole function, so "no calls in the run" means
   "no calls in the function" (matmul's `array(n)` at pc 1 killed it),
   and any real loop pins 3+ scalars so the N5 pool never had 2 free.
2. **Loop-REGION versioning, callee-saved regs + N5 capped at 2**: the
   region [T, L] is a backward branch's span, gates checked over the
   region only (ops BEFORE it - the calls - are harmless, the nav runs
   at the preheader after them); a failed guard jumps to a COLD copy of
   the region alone. This fired - and 46 measured -0.69 instead of -8
   Ir/iter, then 43_sieve +3.3%: the CAP is fragment-wide while the
   benefit is region-local, so every other loop in the fragment paid
   two lost pins. Region-scoped pin RANKING recovered 46 (-6.4%) but
   the sieve stayed regressed - the arbitration was unwinnable.
3. **r10/r11 (the shipped design)**: the hoisted (data, count) live in
   CALLER-saved registers the emitter freed when the N5 cache moved to
   r12-r15 - no reservation, no cap, no arbitration. The price: any
   helper call inside the region clobbers them; `emit_call_epilogue` -
   the single choke point every helper-call emission pairs through -
   re-derives both when a region is active (RAX untouched: it carries
   the helper's status). A sync call would not re-derive, but calls
   cannot be in a region, which is also what keeps r10/r11 unused
   there (only the M5b push emitter touches them).

What survived from the sketch: one base per region, innermost-first,
the preheader guard model (back edges target the post-nav label, so
only the fall-through entry pays it), bounds per element. What did
NOT: the store exclusion - a PLAIN element store never moves a
non-slice base's storage (clone_internal_vec needs the store's own
base to be a slice, which the entry guard excludes; the aliased-slices
clone detaches the VIEWS; growth is a builtin call, refused), so the
whitelist admits the plain store family and 46's `row[j] = s` no
longer poisons its loop. The `fr_immutable`/LICM machinery predicted
here was never needed: run-shape soundness (whitelist + def-scan +
no-jumps-in) replaces dataflow invariance entirely.

MEASURED (callgrind Ir, OPT=1 ASSERTS=0, scale-1-vs-3 delta):
46_matrix_mult **-12.0%**/iter (~89.5 -> ~78.7), 14_array_subscript
**-15.9%**/iter; 01/03/07/18/62/fib all within +-0.01%; 15 (a runtime
slice base every entry - the cold twin runs) exactly neutral.

**C1b - the STORE side. LANDED 2026-08-03**, with two design points
that beat the sketch above:
- No pinned shobj and no third register: the hash byte is invalidated
  ONCE at the preheader (setting hash_valid=0 early only means
  "recompute later" - unobservable), so the per-element store is
  bounds + raw write off the same (data, count) pair.
- The store guard set (const slot / readonly / has_slices - all
  region-stable) is emitted ONLY when the region stores to the base
  (`HoistRegion::has_store`): unconditional store guards would send a
  read-only loop over a CONST base cold, losing C1's read hoisting.
Plus MULTI-REGION: 43 has three hot loops and a one-region pick served
only the tiny fill; regions are now greedy innermost-first,
non-overlapping, sorted by T for the emission walk, r10/r11 reused
across them (disjoint lifetimes).

MEASURED: 14_array_subscript a further **-29.0%**/iter (69.0M ->
49.0M per scale; cumulative -40% across C1+C1b); 46/15/18/01 neutral.
**43/56_sieve still do not move, and the mechanism is now precisely
known** (live-counter proof: g_jit_store_fast 3.1M - the ordinary #92
tier serves them - and g_jit_hoist 0): their arrays are BOOLS
(`primes[i] = true`), the pick stamps StoreElemInt candidates kind
INTS (the Instr does not carry the element kind - StoreElemInt serves
both), and the runtime kind guard sends every entry to the cold twin.

**C1c - the BOOLS kind. LANDED 2026-08-03, design (b)** (the
maintainer's pick): a compile-time ELEM-BOOL hint in the previously
free opflags bit 6 of StoreElemInt, stamped by codegen when a PLAIN
bool-LITERAL store compiles - the checker rejects int->bool, so bool
arrays only ever receive bool values, and the one mislabel (#96's bool
into an int-joined array) fails the runtime kind guard and goes cold:
the hint is ADVISORY, semantics never depend on it. No .myv version
bump: the opflags byte was always stored whole, so the bit rides. The
pick maps a hinted store to kind 3 (bools): the preheader guards
kind_bools, the count is BYTES (no sar - like general), and the
hoisted store is a BYTE write (`mov [r10+r9], dil` - the value is
always a 0/1 literal, since only literal bool values reach
StoreElemInt).

MEASURED: 43_sieve **-45.3%**/scale (150.4M -> 82.3M), 56_sieve_bool
**-44.3%** (216.9M -> 120.7M); 46/14 byte-identical. The stride was
sabotage-verified with maximum volume (8-byte stores into the byte
array: ASan SEGV).

**The READ-side hint LANDED (2026-08-03).** The truth source moved to
the INFERENCER: `Subscript::elem_bool`, stamped beside base_array from
the base's static type (array<bool>, non-opt elem), cloned with the
node. The STORE site switched to it too - one source of truth, and it
removes the value-heuristic's #96 mislabel entirely. LoadElemInt
carries the hint; the pick maps it to kind 3; the hoisted read is
`movzx eax, byte [r10+r9]` - identical int semantics to the ordinary
bool tail. The former kind-CONFLICT (a bool store + read of one base
in one region) now AGREES on kind 3 and hoists both ways - the pinned
test flipped to assert exactly that; the read stride was
sabotage-verified (an 8-byte read of packed bools diverges).
HONESTY NOTE: the hint reaches NO bench - 57_bool_reduce, the corpus's
bool-read loop, is the `if (arr[i])` FUSION (JumpUnlessElemInt), which
is not a candidate op. The value is the conflict resolution + plain
bool reads (reach, like prep).

Remaining follow-ups on the C1 family, in value order:
  - the FUSIONS as hoist candidates (JumpUnlessElemInt /
    ForStepElemInt: hoist-aware base gates + element reads) - that is
    where 57_bool_reduce's 360M/scale sits, and 43/56's count loops;
  - the hoisted-COMPOUND store form (compound ops keep the ordinary
    tier inside a region).

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
