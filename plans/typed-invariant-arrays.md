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

**INCREMENT 1 (the ref_slots narrowing) LANDED 2026-08-04.** The
return-path lever from plans/cpp-gap-extremes.md: `ParamDesc::
proven_type` (i/f), stamped by the ONE-SHOT inferencer for an
un-annotated param that can only receive that scalar - concrete,
non-opt, non-dyn, on a non-template, never-value-used function in the
global scope - and consumed by codegen's ref_slots param join. The
enumeration of bind paths (the #96 discipline): direct calls are
compile-checked; indirect/dyn/callback/builtin paths all require a
VALUE use, which the gate excludes; `$` is not an identifier char and
the reflection builtins return names, so instances leak no other way.
FOUND + FIXED: the value-template redirect never marked the clone's
sym value_used - a value-instantiated instance (`ops[k]`) would have
stamped despite being dyn-launderable; the ops-array gate test pins
zero exclusions. The audit net is the EXISTING pop_window
every-slot-trivial re-scan (VM_HARDENING; on in every CI release
lane) - the force-stamp sabotage aborts -rt. Engagement proven by
g_ref_slots_proven_excluded (TESTS). myv v11 (the ParamDesc byte).
MEASURED (Ir/scale, OPT=1 ASSERTS=0): 10_recursion_deep **-7.1%**,
63_closures -0.7%; 11 flat (LAMBDAS are not covered - their descs are
not global-scope syms; a scoped follow-up), 76 flat BY DESIGN (its
funcs are value-dispatched - the gate refusing is the soundness
working), 08/09/46/01 byte-flat.

**INCREMENT 2 (the TYPE-ELIDED slots) LANDED 2026-08-04.** A
qualified-but-unpinned local (the pool overflow + the sub-threshold
picks - same bad() soundness as a pin, minus the register) skips the
per-write TYPE store; every exit's flush stamps the singleton once
(`Emitter::tflush`), the barrier bracket restores it before any
helper that reads full values, and an elided float slot's READ skips
emit_float_load's 3-way dispatch (provably t_float - the
guard-elision win). THREE holes found by the suite while landing, each
now pinned + sabotage-verified:
- a slot used ONLY by ReturnV (an ARRAY result!) qualified - the >= 3
  pin threshold had silently protected the pool from that, as its own
  comment says; elision has no threshold, so the gate is now
  "int-WRITTEN in the run" (idst);
- MoveV's cache-aware source is a FULL-VALUE memory read that
  propagates the stale type word - sources leave both elision sets
  (`full_read`);
- the barrier bracket fired only when the INT cache was non-empty -
  a closure-capture snapshot of an elided dyn local read a `none`
  type (spurious TypeError); the bracket now fires for float pins and
  tflush too (float pins had been accidentally safe via the call
  prologue's payload spill).
Execution-proven by g_jit_telide; the test shape needed runtime()
armor (a const-arg pure call folded WHOLE - the trap list's #2, hit
again). MEASURED: the corpus is BYTE-FLAT (hot writes are pinned or
lever-A-forwarded already); a 6-hot-accumulator probe - more int
locals than the 4-wide pool - reads **-6.8%** whole-program. Reach +
architecture, like prep.

**Lambda-param coverage was assessed and DROPPED as vacuous**:
11_closure_counter's closure has NO params (its cost is the call
record + capture RMW), and every other corpus lambda is passed as an
argument - value-escaped, which is exactly what the C3 gate must
refuse. The sound cases (a var-bound, calls-only, never-passed lambda
with params) do not occur in real code; recorded rather than built.

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

**THE FRESH PROFILE (2026-08-04, per this plan's own discipline).**
Callgrind Ir/scale, my vs `g++ -O3` twins (bench/cpp, rebuilt):
10_recursion_deep **18.2x** (53.8% fragment + ~35% in the jit_ret C++
round trip - the return protocol is nearly the whole gap),
64_struct_create **10.0x** (89% fragment - dst tag two-stores + guard
loads), 46_matrix_mult 9.3x (vs VECTORIZED C++ at 8.2 Ir/iter; ~3x vs
scalar), 54_mandelbrot **7.3x** / 55_float_sum **6.3x** (86-95% inside
the fragments - the float class), 62_dict 5.3x, 43_sieve 4.5x,
01 3.3x, 14 3.1x, 03/07 ~1.9x.

**C4a-i (float read-dispatch elision) LANDED - measured ~FLAT, and the
NEXT SESSION FOUND OUT WHY: it was serving almost nothing.** Two
defects, both mine, both invisible to every net we had:
  (a) the temp entry gate read `fwd_lout[entry]` - jit_fwd_info exports
      live-OUT, and the test wants live-IN ("does a value from outside
      reach this pc"). At a run head whose first op writes the temp,
      live-out trivially contains it. `jit_fwd_info` now exports the
      fixpoint's own `live_in` too;
  (b) far worse: **`visit_use_def` never knew the B1/B2 SPECIALIZED
      arith family** (IntAddRR .. FloatMulRI, 23 opcodes). That table
      is consulted by the peephole and compute_ref_slots, both of which
      run BEFORE specialize_arith_ops - so nothing noticed. But
      jit_fwd_info runs at JIT TIME, on specialized code, where an
      unknown op is a use-def BARRIER: `lin = all`, every temp live at
      every pc. So BOTH JIT-time liveness consumers were silently inert
      - C4a-i's gate refused every temp, and lever A's `skip_write` (its
      own CLAUDE.md note already suspected it: "defense in depth today")
      could never have fired either.
The motivating "23 dispatches per iteration" figure was ALSO wrong (a
static whole-dump count never verified executed - the prove-the-code-ran
rule violated by its own author), but the mechanism was right and the
measurement was reading a broken gate.
MEASURED once fixed (Ir/scale, OPT=1 ASSERTS=0): 55_float_sum
**-15.0%**, 40_math_builtins **-3.2%**; 54/04 byte-flat (their float
locals were already pinned by C2a), every int bench byte-flat
(`skip_write` stays blocked there by ref_slots' conservatism - the
throttle lever A already documented). PINNED by
`jit_fread_temps_audited` + a compile-time `g_jit_fread_temps` count of
admitted TEMPS - `g_jit_fread` cannot see this, it bumps per fragment
ENTRY and the two LOCALS alone satisfy it. Removing the family from the
table again fails that test.
THE LASTING LESSON: an audit table can be complete for the passes that
existed when it was written and INCOMPLETE for a pass added later at a
different pipeline stage. `visit_use_def`'s barrier fallback is
"correct" (conservative), which is exactly why nothing failed - the
cost was silent and unmeasurable from outside. Any new consumer of an
audited enumeration should assert the table COVERS its input.

**THE REAL C4 DECOMPOSITION** (55 at 107 Ir/iter vs C++ 17): no
single dominator - 9 FloatBins each pay temp two-stores (~14/iter),
literal movabs+movq re-materialization (~8), div zero bit-tests (~9),
plus per-op movsd traffic. C++ keeps the WHOLE expression in
registers. The route, in increasing depth:
  1. **C4a-ii: float forwarding** - LANDED 2026-08-04, see below;
  2. **C4b: expression-DAG registerization** - increments 1 and 2
     LANDED 2026-08-04 (literal registers + register arith sources;
     the result picks its register), see below; a full allocator is
     still open;
  3. **C4c: the return protocol** (10's ~35% jit_ret round trip) -
     LANDED 2026-08-04, see below;
  4. **C4d: struct dst tags** - INVESTIGATED 2026-08-05 and found
     ALREADY DONE; RE-SCOPED to the ctor-dominated member read and
     LANDED the same day (-17.1% on 64), see below. The ctor's own
     guards remain - they need loop versioning.
Each is its own measured increment; 2 and 3 are the big ones.

**C4a-ii LANDED (2026-08-04): float forwarding** - a float producer
hands its result over in XMM0 (where the emitted shape already leaves
it) and a dead temp skips the store; the b-operand case, which is
EVERY corpus pair, moves it aside to xmm1 first. Whitelist = the
arithmetic family on both sides, chosen because those ops have no slow
tier that rejoins after writing the dst. MEASURED: 54_mandelbrot
**-17.4%**, 55_float_sum **-15.4%**, 04_float_arith **-13.9%**,
40 -0.6%, 46/01 flat; 55's hot path 67 -> 57 instructions/iteration.
Note 54 and 04 gain MOST although the read-elision fix left them
byte-flat - their float locals were already pinned by C2a, so
everything left to win was exactly this temp round trip.

**A NON-TASK, recorded so it is not re-opened:** the div0 raise-convey
call's float-pin spill/reload was reported (by me) as sitting on the
hot path. It does not - `raise_convey_unless` emits the whole convey
block AFTER the pass-jcc, so the bracket is cold by construction, and
the `jne` over it in the disassembly proves it. The claim came from
reading a linear disassembly as a linear execution path; the same
error class as the "23 static dispatches" figure. CHECK THE JUMP
TARGETS before costing anything read out of `-vdj`.

**What 55's 57-instruction hot path still holds**, measured by walking
the fall-through path (not by reading the dump top to bottom):
  - **2 temps x 3 instructions** - r5/r7, the two chain positions whose
    consumer is NOT the adjacent op (`x / (x+1)` is consumed two ops
    later). An extension to non-adjacent pairs needs a real register
    allocator, i.e. C4b;
  - the type stores C3 elided only for LOCALS - **DONE as inc 3, same
    day** (see below);
  - **3 float literals at 2 instructions each** (`movabs rax, bits` +
    `movq xmm, rax`) - a rip-relative constant pool would halve it.

**C3 inc 3 LANDED (2026-08-04): TEMPS join the float type-store
elision.** The gate is C4a-i's read gate plus **!ref_listed**, because
the flush stamps t_float at every exit including one before the run's
first write, and a temp - unlike a local, whose pre-decl window holds
`none` - can hold a reference an earlier run left in the frame;
stamping over it hides it from pop_window's release scan. The guard
FIRES on real code: `main` reuses its low temps for the argv subscript
and for a float chain, so 55's r5/r6 stay while r7/r8 elide. Dropping
it fails the suite AND trips LeakSanitizer (watched). MEASURED:
55_float_sum **-1.3%**, 40 -0.6%, rest byte-flat - small ONLY because
C4a-ii went first and had already removed 5 of the 7 writes; the
non-adjacent-temp probe reads **-3.1%** (64 -> 62 instructions/iter).
An ordering note worth keeping: doing forwarding before elision made
the second increment look weak. The pair together is what matters -
55's hot path went **67 -> 55** across the two.

**55's remaining 55 instructions/iteration**, for whoever picks up
C4b: 2 non-adjacent chain temps still round-trip (they need a real
allocator, not a peephole), 3 float literals cost 2 instructions each,
and the rest is the arithmetic itself plus the counted-loop tail. The
int type dispatch on `i` at the loop head is correct (an int promoting
into a float expression).

**C4b increment 1 LANDED (2026-08-04): literal registers + register
arith sources.** The hot-path breakdown above is what chose it - the
literals were the single biggest REMOVABLE item (the div0 guards are a
language semantic and the arithmetic is the work). Two halves:
`farith` takes its source register (so a pinned local or literal is
read in place, no `movsd xmm1, ...` first), and a per-run literal pool
in xmm2/xmm3 loaded once at the fragment entry.
MEASURED: 04_float_arith **-25.0%**, 55_float_sum **-9.2%**,
54_mandelbrot **-6.4%**; 40 flat (its loop calls libm, the gate
declines), 46/01 byte-flat.
THREE things this cost, all recorded because each is a general trap:
  1. the gate must be **LOOP-SCOPED**. A whole-run scan declined 55's
     real bench shape (main's calls sit before the loop) while the same
     loop in a function pinned - the fix moved the measurement from
     -2.6% to -9.2%. Since delete-originals, "the run" is a whole
     function; per-iteration cost questions must be asked per LOOP;
  2. **correctness belongs in emit_call_epilogue, not the gate.** A
     call can be runtime-conditional (a ref-listed float store) and no
     opcode scan can see it. Verified by disabling the gate entirely
     and watching the suite stay green;
  3. materialise through **rcx, never rax** - the epilogue runs
     immediately before every call site's `test rax, rax`.
And it surfaced a LATENT C4a-ii bug: jit_put_float takes its value in
xmm0 and clobbers it, so the ref-listed cold arm destroyed a forwarded
value. The int side had `keep_rax` for exactly this from the start; the
float twin shipped without it. Fixed as `keep_x0`.

**C4b increment 2 LANDED (2026-08-04): the result picks its
register.** SSE2 is two-operand, so one operand must occupy the result
register; pinning that to xmm0 forced an aside-move for every
forwarded temp (which is every pair in a chain). `farith` takes its
destination now and `emit_float_operands` returns the {dst, src} pair,
so the two scratches ALTERNATE down the chain. MEASURED: 55_float_sum
**-6.7%**, 54_mandelbrot **-3.1%**, 40 -0.8%; 04/01/46 flat.

IT COST A REAL BUG, and the lesson is about the NET, not the bug:
jit_put_float takes its value in xmm0, and the ref-listed float store
passed whatever register it was handed - so a non-xmm0 result stored
STALE xmm0. `-rt` was green; bench/my/55 was off by 1.5. That is the
SECOND time in one day (the first: C4a-ii's missing keep_x0 reload)
that a corpus program caught what the suite could not. **The corpus
differential - tw vs the default engine over bench/ + samples/, 83
programs - is now part of the routine.** Reproducing it as a test took
real work, because the trigger needs a reference living in exactly the
temp whose op has a forwarded b, and that depends on SLOT ALLOCATION:
a local-bound array shifts the numbering vacuous, `argv` is empty under
-rt, `clone([..])` misses and `dynarray([..])` hits. So the test
asserts an emitted-code counter (`g_jit_fstore_movx0`) as well as the
value - the coverage is provable rather than lucky, and if a future
change stops the shape covering the arm, the test says so instead of
passing quietly.

**Still open - a FULL allocator** over the float expression DAG: today
the two scratches alternate, which handles chains, but a value that
must live across an intervening op still round-trips through its slot
(55's two non-adjacent temps), and operand `a` still costs a copy when
it lives in a pin (SSE2's two-operand form makes that irreducible
without AVX's three-operand VEX encodings). Widening the pools past
xmm2/xmm3 wants REX-encoded xmm8-15 - the current encoders are xmm0-7
only.

**C4c LANDED (2026-08-04): the INLINE frame pop** - the return-side
twin of M5b's inline push (emit_ret_native, jit.cpp; jit_ret is the
slow tier every guard declines to, before any mutation). Three
measurement-driven course corrections, each its own lesson:
  1. the emit gate started as `ref_slots.empty()` and the counter read
     ZERO on the recursion test - a recursion body's call dsts are
     always ref-listed, so the empty gate excluded the motivating
     shape (prove-the-code-ran, again). The gate became "short list +
     a per-slot triviality guard" (the scan's own check as a decline);
  2. guards-first-by-cheapness was WRONG: the callback benches
     (34/35, boundary declines per element) read +2.4-3.1% paying the
     ref-guard walk before the boundary decline. Order is
     DECLINE-FREQUENCY - and better, the boundary case became its own
     inline ARM (flow->value copy + flow->type = ret, no pop - the
     C++ owner pops), flipping 34/35 from +1.4-1.5% to -15.6/-17.1%;
  3. the ref-slot GUARDS then became the release ARM (a cold
     jit_release_slot call per live reference - pop_window's exact
     per-slot assignment), flipping 76 from +1.5% to -7.9%: a frame
     holding live references now pops inline too. A reference RESULT
     still declines (a raw move would leave the slices set pointing
     at the dying slot - the jit_bind_ref_arg lesson).
Sabotage record: dst guard = LSan leak, ref handling = ASan UAF,
boundary guard + seg-top restore = aborts, ALL watched failing;
UNPROVABLE in isolation (kept as defense in depth): the cache_key
guard (subsumed - cache_key implies a stashed caller_cache today), the
release call (a skip only delays the release to slot-reuse/teardown),
the boundary flow-old guard (every consumer moves flow->value out).
MEASURED (Ir/scale, OPT=1 ASSERTS=0): 10_recursion_deep **-27.8%**,
11 **-18.6%**, 35 **-17.1%**, 34 **-15.6%**, 63 **-8.2%**,
76 **-7.9%**; 09_fib +0.18% (cached returns decline by design);
08/01/46 byte-flat. The remaining return-path residue is the sync call
SITE's sentinel round trip (call rdx -> ret -> cmp rax) and the
depth/nstack bookkeeping - a direct fragment-to-fragment continuation
(the model-flip deferred piece) would be the next, deeper cut.

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


## C4d: the scoped step was already done - what 64 actually costs

**MEASURED FIRST, and the scope was wrong.** C4d was written as "the
C3-elision idea applied to the ctor plans' dsts", from an older
CLAUDE.md line describing 64_struct_create's residual as "type-tag
two-stores + ref-checks per dst". The emitted code says otherwise:
**the planned StructCtorV stores NO dst tag and no dst payload at all**
(`grep` for a dst `.type` store at either ctor site in `-vdj`: zero).
H1's reuse path had already removed them - its four guards PROVE the
slot still holds the same same-def instance, so the type word and the
object pointer are already correct and only the struct's BYTES change.
There is nothing to elide.

**What 64 really costs: 234 Ir per iteration** (callgrind scale-delta,
OPT=1 ASSERTS=0) for one int-POD ctor + 2 field reads, one float-POD
ctor + 3 field reads, and ~6 arithmetic ops. The ctor fast path is 13
instructions BEFORE any field store:

    mov rax, p.type            ; \
    movabs rcx, <t_struct>     ;  | guard 1: dst holds a struct
    cmp rax, rcx / jne slow    ; /
    mov rax, p                 ; \
    movabs rcx, <def>          ;  | guard 2: same def
    cmp [rax+def], rcx / jne   ; /
    cmp [rax+rc], 1 / jne      ;    guard 3: sole owner (H1)
    cmp byte [rax+ro], 0 / jne ;    guard 4: not readonly
    mov r9, [rax+bytes]        ;    the byte buffer

x2 ctors = ~26 of the 234. **Every one of those guards is
LOOP-INVARIANT**: the dst holds the SAME reused StructObject each
iteration, the def is a compile-time constant, and neither readonly nor
the refcount changes inside the loop.

x2 ctors = ~26 of the 234. But the 234 also contains FIVE baked member
reads (`p.x`, `p.y`, `v.x`, `v.y`, `v.z`), and each of those re-checks
guards 1 and 2 - the SAME two guards, on the SAME slot the ctor above
it just wrote - for 8 instructions before it can touch a byte. That is
~40 Ir, a bigger and much safer half, and it is what shipped.

### C4d LANDED (2026-08-05): the ctor-dominated member read

`jit_struct_facts` (codegen.cpp) - a forward MUST dataflow whose facts
are (slot, def) pairs. GEN at a planned StructCtorV on BOTH arms (the
emitted fast path rewrites only the reused instance's bytes; the slow
tier `vm_struct_ctor_planned` either reuses that same-def instance or
puts a fresh `def` one in the slot - neither can leave anything else
there, and neither throws). KILL on any write to the slot, from
`visit_use_def`, with its "an unaudited op is a BARRIER" contract. MEET
= intersection over predecessors (`visit_pc_fields` + the fall-through
edge). BOTTOM at pc 0, handler/finally pcs, every per-pc entry stub and
every unreachable pc; the iteration starts at TOP so a loop-carried fact
survives the back edge.

A proven read emits three instructions and NO slow arm at all - the
guards were its only decline path. Measured 234 -> 194 Ir/iteration
(**-17.1%**), everything else byte-flat cross-binary. Lever `mfact`.

**Still open - the CTOR's own guards (~26 Ir).** At the loop head the
fact dies in the meet between the preheader (the slot is not
constructed yet) and the back edge, and that is CORRECT: iteration 1
really must check. Reaching them needs loop VERSIONING - a preheader
that verifies once, the byte-buffer pointer live in a register across
the loop, and a cold twin of the region for a failed guard: exactly
C1's structure with the ctor's dst slot playing C1's base, plus a scan
proving nothing in the region rebinds the dst or takes a reference to
it (a MoveV copy raises the refcount and H1's sole-owner guard must
then fail). A real mechanism, not a peephole - hence recorded rather
than half-built, as before.

The other ~170 Ir are the field reads' remaining loads, the arithmetic,
and the loop tail - N7 territory, not struct-specific.