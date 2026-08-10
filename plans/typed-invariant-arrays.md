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

**C1d - the FUSIONS as candidates + TEMP bases. LANDED 2026-08-03.**
Three unlocks, each found by chasing where the previous one's reach
ended (the census tool: `-nj -vd` - a plain `-vd` is POST-DELETION,
where a fused run is a bare enter.nat, so the first corpus census of
the fusion ops read zero everywhere):
- `JumpUnlessElemInt` as a candidate (base/idx/hint ride the
  mutated-in-place load's fields); the hoisted read went into the
  SHARED `emit_elem_int_read` so the op's emit needed no change.
- `ForStepElemInt` as a candidate - the fusion COPIES the step's
  struct, so the load's ELEM-BOOL hint transfers by hand after
  set_b_dual; the hoisted form skips the base gate (preheader proved
  it; nothing bail-able precedes the step -> no double-step hazard)
  and declines only the post-step read, to the existing
  jit_elem_int_value tier. Its def list wrongly marked `target` (a
  BRANCH PC) as a written slot - fixed.
- TEMP bases admitted (a foreach-over-array's base is a snapshot
  TEMP): the base is only READ at the preheader, the def scan kills
  in-region rebinds, aliased stores cannot move storage. This is what
  gave 18_foreach_array its first hoist.
- `DictLoadInt/Float` whitelisted (weaker than the admitted
  DictStore); 68_nested's ForStepElemInt regions unblocked.

MEASURED (callgrind Ir/scale, OPT=1 ASSERTS=0): 57_bool_reduce
**-41.6%** (360.6M -> 210.6M), 18_foreach_array **-37.1%**,
56_sieve_bool **-24.8%**, 43_sieve **-18.2%**; 68_nested exactly
neutral (tiny foreach arrays - the regions hoist, the loops are cold);
46/14/01/02/09/15 byte-flat. Sabotages (each watched failing): the
hint transfer dropped -> the kind guard goes cold and the counter
assertion fires; both hoisted reads' byte stride forced to 8 ->
value divergence; the bounds check dropped -> the negative-index
case diverges.

**C1e - the hoisted-COMPOUND store. LANDED 2026-08-03.** The C1b
hoisted arm now serves `a[i] OP= v` too: value load, the runtime
divisor 0/-1 guards (before the bounds check, as the ordinary tier's
precede prep - a div-by-zero store must throw in the helper without
storing), bounds-vs-r11, then the RMW - `mov rcx, r10` lets the
ordinary tier's [rcx+r9*8] tails serve verbatim, minus the per-element
hash store (done once at the preheader) and the whole nav. The
emit-time refusals (float `%=`, literal 0/-1 divisors) moved ahead of
the arm and hold for both tiers; a hint-3 compound never hoists (a
compound on BOOL storage is compile-unreachable - the ordinary tier's
ints kind guard raises the exact error). Execution-proven by the
arm's OWN counter `g_jit_hoist_rmw` - g_jit_store_fast also counts
the ordinary tier, so it cannot prove this arm ran (the per-shape
attribution rule). Sabotages watched failing: the int RMW op swapped
(value divergence), the zero-divisor guard dropped (a hardware #DE in
the fragment), the float farith swapped (divergence). The -1 guard is
NOT sabotage-provable today: INT_MIN/-1 SIGFPEs the helper too - a
PRE-EXISTING, engine-uniform hole this step's test exposed (task
#103: TypeInt::div/mod and the VM store bodies raw-divide with only a
zero check; even the parse-time const-fold crashes; -fwrapv does not
define division overflow). MEASURED: the corpus is byte-flat per
scale (no suite bench compounds into an element - reach + parity,
like prep); a 1M-compound-store probe reads **-29.5%** whole-program
(64.4M -> 45.4M, ~19 Ir per store).

The C1 family is COMPLETE. Next: C2/C3 below.

### C2 - widen the register cache beyond int slots

**C2a (the FLOAT half) LANDED 2026-08-04.** Hot float locals pin in
xmm4-7 (xmm0/1 stay the per-op scratch): a parallel accounting in
pick_cached_slots (usef/badi/badf), entry loads at the head and every
entry stub, the flush/reload/barrier machinery extended, and - since
xmm are ALL caller-saved - a spill to the slot's payload around every
helper call via the shared emit_call_prologue/epilogue (the C1 spill
discipline; sound because a pinned slot is never memory-read by any op
in the run, so the payload-only spill has no reader until the proper
type+payload flush at a real exit).

THE SOUNDNESS RULE that shaped it: a slot qualifies only when a float
op WROTE it in the run (`fdst`) - a float op can legitimately READ a
definitely-int slot through the promote arm, and pinning such a slot
would movsd its int payload bits as a double. A float-written slot is
t_float from its first write, every other writer is a float op (any
non-float writer disqualified it), and the pre-first-write window's
garbage entry-load is dead by def-before-use - the int pool's exact
argument. ReturnV does NOT disqualify (the emit flushes before
jit_ret); MoveV's SOURCE is float-cache-aware with ZERO weight (the
int pool's four-accumulator lesson replayed: 04_float_arith's
accumulator never pinned because its final str(x, 4) staged an arg
move). MathFnV joined the classifier - previously UNLISTED, one math
builtin disabled pinning for its whole run.

SHIPPED BUG, caught by -rt: e.fcache was not cleared per RUN, so when
jit-ineligible selectors (floor/abs) split a body into several
fragments, fragment 2's epilogue flushed fragment 1's never-loaded
pin into the slot (a wrong 40_math-shape sum from iteration 3). The
per-run clear mirrors e.cache's; the run-split shape is a pinned test
and the missing clear a watched-failing sabotage (with three more:
no prologue spill, no exit flush - aborts the suite - and a
wrong-register pinned read).

MEASURED (callgrind Ir/scale, OPT=1 ASSERTS=0): 04_float_arith
**-28.0%**, 54_mandelbrot **-22.5%**, 55_float_sum **-19.5%**,
40_math_builtins -2.6%; 44/46/43/01/09/34/35 byte-flat per scale.

**C2b (a SECOND base per region) LANDED 2026-08-04.** A region's
second-best candidate hoists into a CALLEE-saved pair from the r12-r15
pool: leftover registers after the int picks, else the two WEAKEST int
pins are DISPLACED when the trade wins - the pair's weight is 12x its
region element-op count (the measured per-element nav saving) against
the pins' whole-run use counts, a comparison conservative in the
displacement's favor being DENIED (the pins' counts overstate their
innermost-frequency value). Callee-saved on purpose: a helper call
preserves the pair (no epilogue re-derivation, unlike r10/r11) and
frag_entry's push machinery covers the save; every region shares one
pair (disjoint lifetimes). All six hoist-aware emit arms go through
ONE lookup (`hoist_match(base, kind)`), and the preheader factored
into a per-base `nav` lambda - any base's failed guard sends the WHOLE
region cold, INCLUDING base1's hoisting (the documented trade: the
body was emitted with both hoists live and cannot partially
deactivate; the pinned slice-base2 test counts base1's bump
preceding base2's failed guard). Execution-proven by `g_jit_hoist2`;
sabotage: base2's nav derived from base1's storage = value divergence
in 3 cases; the pair-not-saved sabotage is NOT provable by the
harness (a register-contract violation bites only if the C++ caller
keeps r14/r15 live across jit_enter, which gcc's frames here do not)
- recorded, like C1e's -1 guard. The initial 8x weight was INERT on
the target (g_jit_hoist2 == 0, the prove-it-ran rule): 46's int pool
is full and the pins' whole-run counts beat 8x1; the measured-nav 12x
fires.
MEASURED (callgrind Ir/scale, OPT=1 ASSERTS=0): 46_matrix_mult
**-3.4%** (both inner-loop bases hoisted - part of the nav saving is
given back by the two displaced pins); 14/43/57/18/01/55 byte-flat.

A's forwarding machinery still feeds the eventual allocator
(a forwarded value is a register-resident value with a one-op
lifetime - the allocator generalizes the lifetime).

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
