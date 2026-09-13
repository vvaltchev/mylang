# THE REGISTER-ALLOCATOR ENDGAME (#96 widened, 2026-08-22)

THE MAINTAINER'S MANDATE, verbatim in substance:
 - The three rax-endgame steps are IN scope and come BEFORE other
   tasks.
 - Asking FOR a specific register is still not OK without a HARD
   reason (ABI: rax holds returns; ISA: cl/rax:rdx; encoding facts
   belong in the COST MODEL, not in per-site preferences).
 - Register pinning must become TEMPORARY: "the same variable MUST
   be able to use multiple registers (if needed) even in the same
   basic block, depending on what's more efficient". The bar is what
   a C compiler does with a 10000-line function.
 - Everything of the same kind as run_may_pin_rax must be found and
   converted - the full inventory is below.
 - Claude has FULL freedom to reorder steps for ease of execution.
   The goal is ALL of it done. This file exists so no step is lost
   across context compactions: RESUME FROM THE PHASE CHECKLIST.

STATUS LEDGER (update as steps land):
 - [x] Census-to-zero milestone (1137 -> 0, plans/jit-registers.md
       entries ap..bb) - the enabling precondition for everything
       here: every emitter consumes register VALUES now.
 - [x] Phase A - the rax endgame (A1-A3 landed; A2's sabotage +
       measurement in the same batch; see the ledger entry)
 - [x] Phase B - the same-kind sweep (B1/B2/B2c/B2c-rdx/B3 all
       landed 2026-08-22)
 - [x] Phase C - the float (xmm) mandate (COMPLETE 2026-08-22:
       census, model, tracker, conversion, floors at ZERO; xmm8-15
       wait on REX-capable float encoders - a capability gap, not a
       policy one)
 - [ ] Phase D - the interval allocator (splitting)
 - [ ] Phase E - retire the transition devices + doc sync

---------------------------------------------------------------------
## THE HONEST INVENTORY - what is still a static table or a fixed
## convention, and which phase kills each

 1. run_may_pin_rax + the coverage gate (jit.cpp ~9500 / ~19660):
    the hand-audited whitelist of "rax-free" op shapes + the
    pick-time check that every such op's target actually got a pin.
    Phase A deletes both.
 2. run_needs_float_tag + the rsi/r8 SINGLETON RESERVATIONS
    (jit_xcache_busy): rsi excluded from the xcache pool
    UNCONDITIONALLY (it may carry the t_int singleton off-arena), r8
    excluded by another run-scanning opcode gate (t_float). Phase B.
    [DONE 2026-08-22: jit_xcache_busy deleted; grant_tag_regs +
    tag_holder(); see B1 below]
 3. THE FWD BUS RESIDUE: g_fwd.res_reg defaults to rax (the tagged
    conv line); a pin-producing op pays `mov rax, r14` to put its
    value on the bus instead of DECLARING r14; lever A's pairing
    guard statically declines rax-pinned runs
    (`!e.reg_holds_pin(RAX)` at the pairing site). Phase B.
 4. FIXED PREFERENCE ORDERS + legacy prefer masks: CACHE_REGS
    {r12..r15}, XCACHE_ORDER {10,11,8,7,6,9,2,1,0}, ELEM_CAND, and
    every alloc_scratch(prefer = 1u << <legacy reg>) discount - a
    byte-identity transition device from the conversion batches, not
    a cost model. Phase D absorbs preference into interval costs;
    Phase E deletes the masks.
 5. THE HOIST/C2b REGISTER CLAIMS: the region's rdata/rcount pair and
    its r10/r11 clobber-mask claim. Regions ask like everything else.
    Phase B (the claim -> a recorded grant); fully dissolved in D
    (hoisted values become ordinary intervals). [DONE 2026-08-22:
    B3 below - the mask entry deleted, the claim is ra.busy +
    claim_mask, eviction-retry covers a pinned pair]
 6. THE WHOLE-RUN PICK ITSELF (pick_cached_slots): one slot = one
    register for the WHOLE run, ranked by whole-run use counts,
    qualified by the audited bad()-rules, no splitting, no per-pc
    assignment. Phase D replaces it.
 7. THE ENTIRE FLOAT SIDE: xmm0/xmm1 hardcoded per-op scratch in
    every float emitter; xmm4..xmm7 a fixed fcache pool; the CENSUS
    NEVER COUNTED XMM AT ALL. Phase C brings xmm under the model and
    the ratchet; Phase D allocates GP and xmm with ONE allocator.
 8. Every reg:conv TAG is by definition a deferred conversion - the
    justified column doubles as the worklist. Phase E re-litigates
    each: isa/abi stay; conv must shrink to genuine fragment
    protocol (the slot-window base, the counter bump's
    zero-footprint bracket) or convert.

 STRUCTURALLY RESERVED AND STAYING (hard reasons, forever): rsp;
 rbx = the slot-window base (one stable base per fragment IS the
 addressing convention every slot access encodes); rbp = the frame
 anchor; SysV argument/return registers AT call boundaries; CL for
 variable shifts; rax:rdx around cqo/idiv/imul; low-8 setcc forms.

---------------------------------------------------------------------
## PHASE A - THE RAX ENDGAME

GOAL: rax is an ordinary pool member; run_may_pin_rax and the
coverage gate are DELETED; the never-executed AccScratch refusal
(alt-grant) and reuse arms run under the whole net first.

### A0. THE ONE REAL DESIGN PROBLEM: a rax pin vs the helper-status
### convention. SOLVE THIS FIRST - everything else in A is mechanical.

The collision: ~60 emit sites do
    call <helper>; emit_call_epilogue(e); test eax, eax
and the comment discipline is "the epilogue runs FIRST - rax
survives it". If rax became a spillable pin like r10/r11, the
epilogue would RELOAD the pin into rax between the call and the
status test, destroying the status. After a helper call TWO values
compete for rax - the ABI status and the pinned variable - and no
reload ORDER fixes that; only ending the pin's interval around the
call does (which is phase D's splitting, and why D dissolves this).

A0 ANALYSIS RESULT (2026-08-22, investigated): there is NO existing
"emits no helper call" classification to reuse -
 - op_fully_native is DELETABILITY (never-exits OR convey-with-own-
   loc): CallBuiltinV is fully_native and obviously calls a helper;
 - op_never_exits admits helper-calling ops too (MoveV calls
   jit_move);
 - a NEW hand list would be an audited table whose stale entry
   corrupts a pin (the DANGEROUS failure direction) - inventing one
   is the exact thing this arc deletes.

DESIGN REFINEMENT (2026-08-22, during implementation): the retry
distills to ONE seam, CONFLICT-EVICT. Emitter::rax_pin_conflict():
if a pin occupies rax -> remove it from the cache list, free bit 0,
set e.rax_conflict. Called from (1) emit_call_prologue (the
status-clobber class), (2) acc_take when rax is busy-BY-PIN (the
staging class - after eviction the take proceeds and gets rax, so
every downstream acc.r==RAX assumption, incl. the idiv ML_CHECKs,
HOLDS on the doomed attempt - no per-arm special cases), (3) the
reuse form's pin branch. The doomed attempt finishes emitting
(tracker-consistent post-eviction; its runtime wrongness is
irrelevant - it is DISCARDED), and the chunk re-emits once with a
global deny flag. rax pins therefore survive exactly the runs where
NO conflicting event fires - the old whitelist's semantic set,
derived structurally from what emission DID. Expected: byte-
identical final emission on the whole corpus (any vdjcmp drift must
be root-caused, not accepted).

THE CHOSEN RESOLUTION - (O) RE-EMIT ON CONFLICT, zero new tables:
the pick may hand rax to a pin optimistically (it is LAST in
preference, so this is rare - it needs 12+ hot slots); emission
proceeds; the FIRST emit_call_prologue that finds a rax pin ABORTS
the run's emission (the mid-run bail path already exists - "a
mid-emission return false kills the run's nativization" is a known,
handled event) and the run RE-EMITS ONCE with rax added to the
denied mask. Sound BY CONSTRUCTION: the decision is made from what
emission actually did, not from a prediction table; the cost is one
wasted partial emission on a rare shape. PREREQUISITE VERIFIED
(2026-08-22): the existing bail path (`emit_ok = false` -> jit.cpp
~20915 `if (!emit_ok) { g_hoist = {}; g_hoist2 = {}; e.b.clear();
return; }`) is a TOTAL DISCARD at the emission function's own level
- the function is simply re-callable. THE RETRY IMPLEMENTATION:
 1. emit_call_prologue: if a rax PIN is in the cache, set a new
    `e.rax_conflict = true` and take the emit_ok=false path (no new
    rollback machinery - the discard already works);
 2. the caller re-invokes the run's emission ONCE with rax added to
    the denied mask (a `for (int attempt : {0, 1})` around the
    existing call, second attempt only on rax_conflict);
 3. an ML_CHECK that attempt 2 never conflicts again (rax denied ->
    no rax pin -> no conflict, structurally).
Also confirm which state OUTSIDE the Emitter the first attempt
mutated (labels/fixups/h_cold vectors passed by reference; remap is
read-only) - clear those the way the discard path leaves them.
FALLBACK if rollback is messier than it looks: v0 ships with rax
denied UNCONDITIONALLY except in runs the pick proves trivially
bracket-free by construction... does NOT exist without a table - so
the real fallback is keeping today's WHITELISTED-RUNS-ONLY behavior
alive behind the deleted gates' semantics until D. Prefer (O).

### A1. Make rax grantable to the PICK behind the existing machinery
 - Remove the unconditional bit-0 deny; instead deny rax only when
   the run fails the A0 call-free predicate.
 - Verify emit_call_prologue/epilogue would never see a rax pin
   (call-free runs by construction) - add an ML_CHECK saying so.
 - Verify flush_cache/exit_pc handle a rax pin (generic store; no
   rax-specific assumption) - read the code, then trust the nets.
 - The fwd pairing guard (`!reg_holds_pin(RAX)`) STAYS in A
   (conservative decline); B2 relaxes it.

### A2. Exercise the never-run arms UNDER FORCE before shipping
 - rax is already LAST in XCACHE_ORDER; MYLANG_JIT_XROT already
   rotates the pool. With A1 in place, the xrot rotation that puts
   rax FIRST is the force lever - no new mechanism. Confirm
   jit_xcache_pins sweeps it in-process and corpus_diff --xrot
   covers it.
 - What must be OBSERVED running (JITSTATS/TESTS counters; add one
   per arm if none exists - "the tier ran" proof, per the
   prove-the-code-ran rule):
     * AccScratch::take() refusal -> alt-grant (today ML_CHECK-dead);
     * AccScratch::reuse_t hitting an OPEN WINDOW (already runs) and
       hitting a rax PIN (must stay unreachable in call-free runs -
       the ML_CHECK is the net);
     * a staging op emitting through the alt register end-to-end
       (vdj inspection of one forced fragment).
 - Battery: -rt both arenas, corpus plain/--levers/--xrot/--cold/
   --nolowmem, nested_fuzz, Net 2/3 samples, rel-hard, clang lane.
 - REMOVE the ML_CHECK(false) from the refusal arm when it becomes
   reachable-by-design (it stops being "gates make this impossible"
   and becomes an ordinary code path).

### A3. Delete the gates
 - Delete run_may_pin_rax, the coverage-gate block, and their
   comments; the two-address emit forms REMAIN (they are
   optimizations, not gates).
 - Grep tests/docs for references (the #162 decline-case tests, the
   "run_may_pin_rax" mentions in comments and docs/CLAUDE.md) and
   update in the SAME commit.
 - Sabotage on the committed tree: make the alt arm grant but emit
   literal rax anyway -> with xrot forcing a rax pin, the tracker or
   the differential must fail BY NAME. Watch it.

### A4. Measure
 - RULE B1 discipline. Expect ~flat (call-free rax-pinnable runs are
   rare); the win is the deleted tables. Record honestly.

---------------------------------------------------------------------
## PHASE B - THE SAME-KIND SWEEP

### B1. The rsi/r8 type-singleton reservations  [LANDED 2026-08-22]
RESULT: jit_xcache_busy DELETED (the last static pool exclusions);
Emitter::grant_tag_regs decides the holders once per run, claims
them as ra.busy before the budget + pick, and store_type_tag /
cmp_reg_tag / cmp_rax_tag LOST their caller-supplied holder
parameter - tag_holder(tag) is the one query, ML_CHECKed against
the recorded grant (the store_dst_bool bug class is an emit-time
abort now). float_tag_live deleted (write-only after this);
elem_reg_usable_nopin consults the grant mask;
check_pins_are_busy() enforces "the reservation IS ra.busy" at
every allocation seam. run_needs_float_tag SURVIVES as the grant's
fail-safe input only. A latent hazard closed with it: the barrier's
redundant `e.ra = RegAlloc()` wiped `denied` for exactly the
barrier'd op's emission - deleted; clear_cache_state keeps denied
AND the grant. MEASURED: 115/116 byte-identical on AND off the
arena (the one drift IS the barrier fix: 73_multi_unpack's
raise-path scratch, rcx->r10, counts equal). NOTE the "pool gains
rsi/r8 on-arena" payoff predicted here was ALREADY the shipped
state - the old exclusions were `!imm`-gated - so B1 is structure,
not registers. Holders still land on rsi/r8 by construction (the
grant precedes the pick); arbitrary placement arrives with D.

 - Original design (kept for context): the singleton holders become
   MODEL-OWNED, RUN-SCOPED GRANTS recorded in the Emitter; every
   site that passed literal RSI/R8/8 consults the query; the
   reservation IS ra.busy; delete run_needs_float_tag's pool role
   and the unconditional rsi exclusion.
 - Oracle: the nolowmem lane + off-arena vdjcmp/xrot (all run,
   all green).

### B2. The fwd bus declares instead of moving  [LANDED 2026-08-22]
RESULT, measured honestly: pure mov MIGRATION, zero net Ir - the
corpus's forwarded pin consumers are all DESTRUCTIVE (accumulator
chains compute INTO the bus value), so the copy the producer no
longer pays is owed at the consumer instead (21 programs drift by
instruction POSITION, every count equal). The value is STRUCTURAL:
res_reg is declared state now, which D's arbitrary-register
producers require, and no consumer may assume the bus was moved to
rax (the read-only consumers - op_rr2/spill/mem - take the pin
directly the day one appears). The div consumers copy a non-rax bus
value into rax (ISA); the destructive consumers guard with
reg_holds_pin. ALSO in this batch: the Phase A gap - IntModRI /
IntAddModRI write rax raw under reg:isa with no ask and no bracket,
so they call rax_pin_conflict() themselves.

 - Producers with a pinned result set g_fwd.res_reg = <pin> and stop
   emitting `mov rax, d` (the publisher move) - consumers already
   read emit_fwd_bump's return. This DELETES an instruction per
   forwarded pin-produced value: measure Ir on the regs_int family.
 - Relax lever A's pairing guard: pairing works in rax-pinned runs
   once nothing assumes the bus is rax. (Guard text at the pairing
   site names the old reasoning - rewrite it.)
 - jit_fwd_deadtemp + the fwd family-coverage test adapt; remember
   the lesson "a test derived from a table cannot find a hole in
   that table" - assert on the DECLARED register, not on rax.

### B2c. THE GENERALIZED SEAM  [LANDED 2026-08-22]
reg_pin_conflict(r) - any register; pin_conflicts is a MASK; the
retry accumulates g_jit_pins_denied and loops (bounded: each retry
denies at least one more register). CONVERTED: the rcx per-op shift
scan in jit_xcache_busy is DELETED - the RR-shift's raw CL load
evicts a pinned rcx itself (the WATCHED 13-pin `sb += sa >> k` shape
now takes conflict->retry and emits identically; vdjcmp 116/116).
[DONE 2026-08-22] run_may_pin_rdx IS DELETED - see below; nothing
of the whitelist class remains for GP registers.
THE THREE FINDINGS its deletion flushed, each caught by a net in
minutes (the tracker, -rt's store-tier value tests, the nolowmem
rotation sweep):
 1. elem_scratch_plan never picked COUNT - literal rdx, covered by
    the whitelist; picking it broke an implicit count==rdx remainder
    dependence AND ate a reservation candidate (two watched value
    failures) - so count STAYS rdx and the store tiers claim rdx at
    ENTRY via the conflict seam instead;
 2. the deny bit DOUBLED as a GRANT filter (the r8 lesson verbatim):
    un-denying rdx let ordinary grants return it, and div_magic's
    keep died at the imul two instructions later (a wrong `k % 2`,
    watched live at ZERO pins). Every ask whose value must survive
    cqo/idiv/imul now EXCLUDES rdx (hold gained an exclude param);
    cqo/idiv_reg/imul_reg are SELF-DECLARING encoders (they call the
    conflict seam at the one place the ISA claim is made);
 3. the ctx-chain table fallback's "refusal implies no pin" argument
    broke under all-pinned pressure - it evicts now.
Old whitelist text (for the record):
Deleting it = every raw rdx writer calls the seam or asks - the BIG
one is the element tiers' ElemRead/ElemScratch role registers
(count=RDX, data=RCX conv defaults used raw across the tiers), plus
the div arms' movabs RDX/cqo/lea (add reg_pin_conflict(RDX) beside
the existing RAX call at IntModRI/IntAddModRI/IntBin-div/UnaryV-dv).
Do the div arms first (mechanical), then the tier roles via ONE seam
in elem_read_plan/elem_scratch_plan (evict any pinned role), then
delete run_may_pin_rdx. NEXT SESSION'S FIRST ITEM.

### B3. The hoist/C2b claims become recorded grants  [LANDED 2026-08-22]
RESULT: the hand-built `clob |= HOIST_REGS_MASK` entry is DELETED.
The region claims r10/r11 at its own entry - a conflicting pin is
evicted via the seam (retry denies it), then the pair joins ra.busy
and Emitter::claim_mask (the generalized run-scoped claim record the
B1 tag holders share; clear_cache_state restores it, so a barrier no
longer wipes a live claim; check_pins_are_busy enforces it). Claim
held to run end = the old deny scope for scratch; region-scoped
release is a D refinement. elem_scratch_reserve still models the
claim at pick time (has_hoist). REACH: no corpus program drives the
eviction - the `jit: B3` -rt case constructs it (7 accumulators +
array walk), asserts retry>=1, watched failing (tracker names r10).
MEASURED: 104/116 byte-identical both arenas; all 12 drifts are
hoist-run scratch substitutions (rsi/rdx -> the freed r10), counts
equal per program. The C2b pair (pair_lo/hi) was ALREADY a recorded
take_fixed grant - no change needed there. The "pool-denied run"
scratch fallback keeps its conv tag until D.


---------------------------------------------------------------------
## PHASE C - THE FLOAT (XMM) MANDATE

### C1. Census first (instrument before change - the standing rule)
 - Extend scripts/regcensus.py to xmm0..xmm15: operand tokens (X0,
   X1, XMM constants), accessor-name derivation (the movq_x/fload/
   movsd/cvt families name xmm registers the way movabs_r9 named
   r9 - the sixth audit shape applies verbatim).
 - Baseline the counts, add xmm floors to regcensus_floor.txt (the
   gate already fails both directions).

### C2. The model learns xmm  [LANDED 2026-08-22]
RESULT: fp_allocatable (xmm2..7; X0/X1 stay conventional until C3,
xmm8-15 wait on REX-capable encoders) + fp_weight; RegAlloc.fbusy +
ftake/ftake_fixed/fgive; alloc_fscratch/free_fscratch/ftake_reg
seams; the C2a pin assignment through the register state
(byte-identical); check_pins_are_busy covers fcache-vs-fbusy;
fwrote() in every xmm-writing encoder, write_fslot's pin arm and the
entry loads declared as machinery - WATCHED failing by name.
116/116 byte-identical both arenas. C1 was the same day: the census
knows X0/X1 + the x0/x1 accessor tokens; floors XMM0=61 XMM1=28.

### C3. Convert
 - [DONE 2026-08-22] the reg:abi batch: the SysV/helper boundary
   sites are TAGGED, not converted - emit_put_scalar_call's xmm0
   marshalling, fmod/MathFn MK_CALL's libm args+return, the
   fcreg->jit_put_float move. Floors: XMM0 61->54, XMM1 28->27,
   TOTAL 89->81. What remains is EXACTLY the staging class.
 - [DONE 2026-08-22] THE REMAINDER WAS ONE SEAM - built same day,
   with ONE simplification over the design below: the pair is a
   RUN-SCOPED grant (grant_fstage, the B1 pattern), not per-op RAII -
   holding it run-long is what makes the float fwd bus sound with NO
   per-op exclude dance (nothing else can be granted the registers a
   forwarded value sleeps in between ops). Every staging site reads
   fsa()/fsb(); the fixed-pair encoders farith_x1_x0/pxor_x1 are
   DELETED (generic farith/pxor_rr); the bare-number xmm element
   operands converted too; fmod ML_CHECKs the stage IS the SysV pair
   (force_x0_x1 conflates "the stage" with "the ABI pair" - a D-era
   stage move must add marshalling there). fp_allocatable now
   includes xmm0/1. FLOORS AT ZERO: XMM0 0, XMM1 0 (8+1 abi-tagged
   sites remain, all SysV). 116/116 byte-identical both arenas.
   The original per-op design, kept for the D-era revisit:
   * FStage: a per-op RAII PAIR ask (mirroring AccScratch) - two
     ra.ftake calls, prefer X0 then X1, freed at op end. With the
     prefer discounts it is byte-identical while xmm0/1 are free,
     which today is always.
   * fp_allocatable currently EXCLUDES xmm0/1 (the C2 note); flip
     that IN THE SAME COMMIT as the first FStage users - the askers
     are the only alloc_fscratch callers, so nothing else can grab
     the pair.
   * ⛔ THE FLOAT FWD BUS RULE: g_fwd.fin_reg carries a value ACROSS
     the op boundary (the producer's fres_reg); a consumer op's
     FRESH asks must EXCLUDE it while the bus is live, or the ask
     clobbers the forwarded value before emit_float_operands reads
     it. Today the fixed X0/X1 dance encodes this structurally
     ("build `a` in the OTHER scratch"); the seam must thread
     exclude = bus-live ? 1u << fin_reg : 0.
   * The clusters: emit_float_operands + its ~28 emit_op consumers
     (farith/ucomisd/sqrtsd/emit_float_store chains), the compare
     ladders (12805/12928/15927/16854/18474), the element tiers'
     float staging (13340-13639, 14809-14891), MK_SSE (14752-59),
     15762, 17444/62. fwrote() is the net - a conversion mistake
     aborts by name (watched in C2).
 - The fcache pool {xmm4..7} registers its picks in the xmm mask
   [DONE in C2 - ftake_reg].
 - ffwd: fres_reg is already a declaration - consumers must READ it
   (audit for assumed-xmm0 consumers, the C4a-ii/C4b family).
 - Ratchet xmm floors batch by batch to zero, same discipline as GP
   (vdjcmp byte-identity per batch, sabotage watched, floors in the
   same commit).

---------------------------------------------------------------------
## PHASE D - THE INTERVAL ALLOCATOR (the C-compiler bar)

THE TARGET, by example. Today `pick_cached_slots` decides "s lives
in r12 for the WHOLE run" or not at all; a 20-hot-variable loop
serves ~7 and the rest stay memory-bound forever. After D:

    lines 1..40    x in r12        (hot stretch)
    lines 41..900  x in its slot   (cold stretch - r12 serves y)
    lines 901..950 x in rax        (hot again; rax free here)

one variable, several registers, decided per LIVE RANGE by cost -
including INSIDE one basic block. Splitting also dissolves A0's
interim rule: a caller-saved interval simply ENDS before each helper
call and resumes after, which is how a C compiler treats caller-
saved registers.

FOUNDATIONS ALREADY IN PLACE (why this is feasible):
 - every emitter consumes register VALUES (#96's threading);
 - jit_slot_liveness computes livein/liveout per pc (the fixpoint
   built for lever A's widening - the dataflow EXISTS);
 - the native-stack spill homes (#96 inc-1) are the spill substrate;
 - take/free + the REGTRACK tracker police occupancy;
 - per-pc entry stubs (M5b/c) already exist - the constraint they
   impose is listed below, not discovered later.

### D0. Baselines and the pressure corpus  [CAPTURED 2026-08-22]
 - THE Ir LEDGER (callgrind, OPT=1 ASSERTS=0 LTO=1, scale 1, sha
   039a30c - build build-claude/perf per RULE B1). Ir is the
   cross-session-comparable metric; the wall-clock A/B at D's end
   must be INTERLEAVED against a baseline binary rebuilt from THIS
   sha (cur/base is the only trustworthy wall number):
     80_regs_int_08   126,250,368
     81_regs_int_14   206,762,340
     82_regs_int_25   354,124,582
     83_regs_int_40   555,077,368
     84_regs_ref_08   320,208,064
     85_regs_ref_25   961,417,427
     46_matrix_mult    87,930,309
     55_float_sum     114,847,876
     04_float_arith    32,088,245
     01_while_loop     24,315,122
     07_nested_loops   42,586,938

### D1. Live intervals  [LANDED 2026-08-22]
RESULT: jit_build_intervals (codegen.cpp/.h) - per-slot maximal
stretches WITH HOLES ({slot, [start,end), weight}), sorted by start.
SPEC (stated in codegen.h, and the -rt check derives from it, not
from the builder): an interval of s covers pc iff
live_in(pc,s) || s in defs(pc); a def OPENS the stretch at the
defining pc. Barrier ops read as use-everything/define-nothing -
conservative-safe. The `jit: D1` -rt check verifies coverage both
directions at every (pc,slot), disjointness, and recounted weights,
with a HOLE vacuity guard (some slot must yield >= 2 intervals);
WATCHED failing (the def-opens rule removed -> coverage mismatches
named by pc/slot). next-use chains stay with jit_next_use for D3.

[D2 LANDED 2026-08-23 - see its section. Validator arm 1 landed the
same day - see D4.]

[D3.b step 1 LANDED 2026-08-23: `pick_visit_op(ck, in, pc, v)`
(src/jit.cpp, above pick_cached_slots) is THE shared per-op
classification switch - a template over a visitor struct with the
ten callbacks (usei/usei_dst/usef/bad/badi/badf/use_ret/fdst_mark/
full_read_mark/mark_barrier; contract in its header comment).
pick_cached_slots binds its accounting lambdas through a local Fns
struct and its loop is now three lines; `return false` = the op is
unclassifiable (the pick caches nothing). The extraction was
comment-aware and count-asserted: 160 call sites converted, every
count checked (usei 28, bad 82, badi 14, usef 14, mark_barrier 7,
usei_dst 6, fdst 5, badf 2, full_read 1, use_ret 1). Verified pure
code motion: vdjcmp 116/116 BOTH arenas, -rt 1690x5 both arenas,
corpus_diff 25/25, driver_checks, census gate at floor, TESTS=1
OPT=1 -rt, clang OPT=1 ASSERTS=0 LTO=0 zero warnings.

D3.b step 2a LANDED 2026-08-23: jit_qualify_intervals (jit.cpp,
IntervalQual in jit.h) - pick_visit_op driven with the interval
side's callbacks, each event attributed to the interval covering
(slot, pc). Raw facts per interval (int/ret/float use counts,
wrote/full_read/mem flags); pool derivations stated in jit.h, left
to the scan. `orphans` = the visit_use_def-vs-pick_visit_op drift
detector, asserted 0. The `jit: D3.b` -rt check pins aggregation
against the pick's public answer (TESTS-only export of the static
pick): A picked => per-interval clean (+ A-float for fhot), D
refused-yet-hot => mem_int somewhere, B the payoff interval
observed + vacuity-guarded, C orphans == 0. WATCHED failing both
ways (mem_int stripped -> D names 4 slots; attribution disabled ->
C + A). Recorded gaps for 2b: uses_ret float-exemption CLOSED 2026-08-23
(function-chunk float-return case, vacuity-guarded, watched via a
shared-switch sabotage); corpus-wide orphan census rides with the
scan.
ADDENDUM: MemEvent stream {pc, slot, gp_only} - the forced-end
POSITIONS the scan cuts on; property E pins stream/flags agreement
both ways (watched: pc+1 stamp fails at four named sites).

2b-i LANDED 2026-08-23: jit_lsra_assign (jit.cpp; contract in
jit.h) - cut at MemEvent pcs (event pc = one-pc forced-memory
piece), admit float-free pieces with a jit_next_use-proven use
inside, walk by start with expire-and-free, evict-furthest WITH
the split (loser keeps its register up to the contested pc,
remainder is a memory piece). Abstract registers 0..K-1; GP only.
The `jit: D3.b 2b-i` -rt net: I1 tiling / I2 no conflicts / I3
forced memory + shapes (payoff piece RESIDENT at K=4 where the
pick refuses the slot; picked slots resident at K=4; K=1 the idle
slot LOSES residency-length to a hot one). WATCHED failing 3 ways
(evict-nearest, unmarked register, cutting disabled). FINDING:
"every picked slot resident" is unsatisfiable at K=1 - pressure
guarantees are COMPARATIVE (who loses), the assertion states that
now. v1 gaps recorded: no split-remainder re-queue (second
chance), float twin, physical binding/cost model - all 2b-ii.

2b-ii OPENING LANDED 2026-08-23: the `lsra` lever
(MYLANG_JIT_LSRA=1 / g_jit_lsra, default OFF) - the scan chooses
the PIN SET (plan reduced to whole-run residency, pick's order),
everything downstream unchanged; any stage declining falls back.
Execution proof g_jit_lsra_pins (JITSTATS). ⛔ ITS FIRST SWEEP
FOUND THE ReturnV TYPE-EVIDENCE RULE: a ret-only closure slot
pinned on weight>0 was flushed t_int over t_func (NotCallableEx +
leak) - the pick's >=3 threshold had been silently carrying the
soundness rule. Now explicit: GP admission needs uses_int > 0;
uses_ret is weight, never evidence. Gated in the scan (evid), the
bridge (wint>0), property F in the 2b-i net (function-chunk case,
watched failing), and the bridge test end-to-end. Coverage tests
that pin the DEFAULT allocator (jit_xcache_pins,
jit_hoist_pair_conflict) force g_jit_lsra=false for their span.
Verified: 2x2 lever-x-arena -rt matrix, corpus both states +
off-arena, vdjcmp 116/116 both arenas for the DEFAULT config,
rel-t both states, clang lto0, non-JIT, census.

2b-iii-a LANDED 2026-08-23: jit_lsra_snap - the plan translated
into the SEAM VOCABULARY (#96 inc-2's pattern, generalized):
entry state + LsraTrans transitions at LINEARIZATION POINTS (the
share plan's own soundness note refuted the marker's earlier
"in-edge fixup moves" sketch - a transition may only sit at a pc
no edge crosses; targeting is legal, which makes a loop-head
install the loop-carried pin). THE CONTINUATION RULE: a piece end
flushes iff the interval continues (mem-cut or split remainder);
death/hole ends drop silently (no lin point needed, and
drop-at-death protects the slot's next def) - so installs never
evict. jit_run_edges/jit_lin_point are the ONE shared edge scan +
crossing test (share plan + snap + net). Net: S1 lin-only, S2
replay==coverage, S2b every continuation end owns its flush
(replay alone cannot see a dropped flush), S3 demotion-only, S4
in-loop events demote; watched failing three ways. Share-plan
refactor byte-identical (vdjcmp 116/116 both arenas).

2b-iii-b LANDED 2026-08-23: TRANSITIONS EXECUTE. The seam loop
gained the split arms (flush = store tag+payload + entry removed +
register given back; install = take_fixed at its pc + entry + load,
both before label[pc]); entry stubs replay entry + trans <= pc;
exits/brackets needed NOTHING (e.cache evolves and is truthful at
every pc). Physical binding: entry occupants ride the zip, mid-run
aregs fix identity + callee-saved push at setup. Execution proof
g_jit_lsra_trans; the phase-handoff shape runs 8 transitions with
the right answer. TWO RULES from the nets: (1) BUSY <=> ENTRY -
per-pc busy (take_fixed at install, give at flush; a busy-but-empty
register was invisible to conflict-evict and hit AccScratch's
abort); (2) TYPE EVIDENCE IS PER PIECE (the d1 finding - a dyn
slot's ONE interval spans boxed redefinitions, which are liveness
barriers; interval evidence pinned an ARRAY as int) - the qualifier
emits int_uses {pc,slot} events, admission needs a touch INSIDE the
piece, property G watched failing names the exact piece. v1 bounds
recorded: hregs/spill-homes decline trans mode, no temps, rax
unreachable for mid-run pieces (pool order). MYLANG_LSRADBG=1
dumps the plan. Verified: full lever x arena matrix + corpus
--levers/--xrot COMPOSED with the lever + vdjcmp 116/116 default.

FIRST LEDGER READ (2026-08-23, same-binary env A/B, OPT=1
ASSERTS=0, callgrind Ir, build-claude/perf): lever-on vs off:
80_regs +0.16%, 01_while +0.28%, 46_matrix +0.59%, 55_float
+0.10%, ⛔ 83_regs_int_40 +6.08%, ⛔ 07_nested_loops +14.29%.
DIAGNOSED (07, via LSRADBG + -nj -vd): the INNER loop counter j is
DEMOTED - its piece starts at pc 12, and the OUTER loop's exit
edge (11->20) crosses every pc of the outer body, so no install
can sit there. The pick never had this problem: a whole-run pin
needs NO transition (entry dead-load, sound by def-before-use).
83 is the same class at scale (40 slots, 13 pins - demotions +
scan-order admission vs the pick's weight ranking).

2b-iii-c inc 1 LANDED 2026-08-23: BOUNDARY EXTENSION.
Extend-or-demote replaces blind demotion (start backward to the
latest legal pc, end forward to the earliest; bounded by same-slot
pieces and reg neighbours; the whole-run pin is the degenerate
extension). MEASURED: 07_nested +14.29% -> +0.23%. The bounds are
DEFENSIVE (two sabotages unreachable by crafted shapes) - enforced
by the translation model as a MACHINE CHECK (register overlap AND
slot-on-two-registers refused at every lever compile; the escape
analysis' reassignment-guard precedent), plus property S5 and two
extension-shape cases in the -rt net.

2b-iii-c inc 2 LANDED 2026-08-23 - and the diagnosis CORRECTED
the scope: 83's +6.09% was not the winner selection or the
eviction but the MISSING SPILL-HOME TIER (lever-off serves 13
pins + 16 homes; v1 zeroed the homes). The merge: trans mode's
overflow (uses_int >= 3, no mem_int anywhere, no float facts, no
resident piece) joins spill_hot weight-ranked via lsra_homes
(appending to hot corrupts the abstract-reg zip - its ML_CHECK
caught the draft); jit_share_plan disabled under trans mode (the
transitions own sharing). MEASURED: 83 +6.09% -> +0.76%; the
six-bench ledger reads +0.14%..+0.76%.

2b-iii-d inc 1 LANDED 2026-08-23: VALIDATOR ARM 2 - label
in-edge agreement as a machine check (per-fixup GP-cache
signatures vs the landing position's, pre- vs post-transition
respected; runs under BOTH modes, so it validates ShareSeams
too). Watched failing: jit_lin_point admitting one crossing
aborts the next lever -rt by name. The check is #ifndef NDEBUG
(validation only) - `want` and then `to_pre` were the cc1f50f
set-but-unused shape, both caught by the pre-push clang
OPT=1 ASSERTS=0 LTO=0 step.

⛔ BROAD-LEDGER FINDING (2026-08-23, 13-bench same-binary Ir
sweep - the six-bench set had a corpus hole): ⛔ 43_sieve +37.27%,
14_array_subscript +2.48%, 81_regs_int_14 +1.26%; everything else
sub-1%. DIAGNOSED (43, via the -vdj entry loads): the lever pins
{i1, n, i2, count} - TWO DISTINCT SLOTS both named i (two `var i`
decls; NOT the S5 hazard) - and LOSES j and k, the inner sieve
counters. Their piece starts sit inside crossed regions, and the
backward extension is blocked by the PREVIOUS piece on their
assigned register (the scan's lowest-free-first packing leaves a
predecessor ending mid-crossed-region, so no legal lin point
exists in the gap; 07's fix worked only because that register
happened to be empty).

2b-iii-d inc 2 LANDED 2026-08-23 - AND THE FIRST THEORY WAS
WRONG: trans mode never engages on 43 (hoist regions decline);
the +37% lived in the WHOLE-RUN FALLBACK, whose all-pieces-
resident rule excluded live-in slots with IDLE PREFIXES (the
sieve's inner counters enter their loop-body run untouched for
the first pcs - no candidate piece there, all_res false, the
hottest slots ran from memory). The fallback is now the pick's
contract restated on interval facts (wint>0, no mem_int
anywhere, no float, >= 3) with NO plan detour (it no longer
calls jit_lsra_assign). MEASURED: 43 +37.19% -> +0.45%, 14
+2.48% -> +0.31%; 10-bench ledger +0.12..1.24% (81's +1.24% is
the residual class). Rider: trans-mode extension gained
REASSIGNMENT (reg-neighbour-blocked pieces move to a free
register over the span) - machine-checked, green, REACH
UNVERIFIED (noted, not claimed).

NEGATIVE RESULT, RECORDED (2026-08-23): a SPLIT-WORTHINESS FLOOR
(evict-whole when the kept prefix has < 2 int touches, restoring
home eligibility) was built on the theory that 81's +1.24% was a
shredded evictee - MEASURED FLAT on its target (81 unchanged to
the second decimal) and UNPINNABLE by the current cases (the
evict-nearest sabotage went green after the K=1 property was
restated on qual weight - the idle slot's own 3 events make it
"hot" to that formulation - and no case distinguishes the floor
at all). REVERTED to the committed, watched state per the
unmeasurable-unpinned rule. What a re-attempt needs: the ACTUAL
81 diagnosis first (per-iteration scale delta + the loop-body
-vdj diff, the tools that settled 43 - NOT plan-dump theorizing:
two theories in a row were wrong), and a case that observes the
evictee's placement directly.

⛔ THE MAINTAINER'S REGRESSION AUDIT (2026-08-23, raised over the
my/cpp geomean reaching 2.225x): ANSWERED WITH THE TRUSTED
INSTRUMENT. RULE-B1 interleaved wall A/B, HEAD vs the D0 sha
(039a30c), both binaries built this session, header verified:
⛔ cur/base geomean 0.999x over 87 benchmarks - THE D-ARC'S
DEFAULT CONFIG IS WALL-FLAT, and the emitted code is proven
identical three independent ways (vdjcmp per commit;
scale3-scale1 per-iteration Ir bit-equal on every ledger bench;
the generated-code Ir row byte-equal at function level:
407,000,371 both on 83). my/python geomean 13.56x; the cpp-cache
machine marker shows no drift.

BUT THE SUSPICION FOUND SOMETHING REAL AND SMALLER: whole-program
Ir crept +0.1-0.3% (83: +2.0M, mostly-monotonic across the arc's
byte-identical commits) - 100% of it __strcmp_avx2 called from
libstdc++'s RTTI (typeinfo name-compare fallback), i.e. COMPILE-
phase cost that wobbles with binary layout/linkage per commit
(getenv counted flat at 21 calls; every MyLang function row
byte-equal). Below wall resolution today; real; it noises any
my/cpp reading that includes compile. AND IT EXPOSED THE BIG
PRE-EXISTING FACT: on 83, compile is ~148M Ir of which ~125M is
dynamic_cast/RTTI + its strcmp - COMPILE IS ~84% RTTI. The pass
pipeline's dynamic_cast chains are nearly the whole compile bill:
a first-class optimization target (the tag-check pattern the
codebase already uses for is_id/is_lit_int), and the amplifier
that turns linkage wobble into measurable drift.

THE LONGER-WINDOW my/cpp GROWTH IS NOT THE D-ARC. Remaining
suspects, in order: task #99's TWO REAL pre-D0 Ir regressions
(the 2026-08-16 regression check - still open, now the priority
candidate for the step-by-step hypothesis), and the earlier
cpp-bench re-timing. Bench-infra notes from this audit:
28_str_concat's PYTHON side times out at the current scale and
78_typed_param_call failed to time in the recompute - both need
a look (infrastructure first).

[#99 CLOSED 2026-08-23 (both regressions attributed; the 67 argv
copy fixed, -3.04%); #96 RESUMED. 2b-iii-d inc 3: 81 diagnosed to
ONE instruction/iteration (a retry-denied register -> a 1-use-
prefix eviction -> home eligibility lost -> frame + type stamp);
the split-worthiness floor VINDICATED (the earlier "measured
flat" was a false reading - re-measured: 81 per-unit Ir becomes
IDENTICAL to lever-off) and re-landed with property H watching it
by name. Ledger: +0.20..0.45% across the spot set - compile-side
analysis cost only. OPEN: the evict-furthest policy has no
working watched case (three formulations failed; temps dominate
K=1 dynamics) - a temp-aware pin is a follow-up.

FULL-SUITE LEVER SWEEP (2026-08-23, same-binary Ir, scale 1, all
87): the tail names three REAL per-iteration regressions (scale
split confirms loop-side, not compile): ⛔ 69_exc_crossframe
+4.13%/iter, ⛔ 34_sort_custom_cmp +3.88%/iter, ⛔ 44_primes_sqrt
+3.21%/iter; then 73_multi_unpack +1.81%, 86/87 elem compounds
+1.15% (scale-unsplit), everything else < 1%. Three fresh
diagnoses, each via the 43/81 toolkit (loop-body -vdj diff +
LSRADBG plan dump; evidence before theory - twice now the first
theory was wrong). The shapes hint: 69 = exception-heavy (the
raise path's cache interplay?), 34 = callback loop (brackets/
transitions around VmInvoker?), 44 = sqrt loop (float/MathFnV
interplay with GP pieces?) - but DIAGNOSE FIRST.

SECOND CHANCE: analyzed and NOT BUILT, on reach grounds (the
whole-span rescue provably never fires - a span denied at arrival
stays blocked; the partial-rescue split has real complexity and
~zero reach on every known ledger shape: nothing frees mid-span
at these pressures). Re-open only when a ledger bench demands it.

ALL FOUR DIAGNOSED (2026-08-23, the maintainer's flatness
mandate: "no theoretical reason to be worse than flat" - correct,
and the mechanisms prove it):

⛔ 69_exc_crossframe +4.13%/iter: NOT a codegen defect - the
emission is BYTE-IDENTICAL under the lever (zero -vdj diff
lines), a dummy same-length env var reproduces nothing, and the
whole delta is _int_malloc doing 2.3x the WORK PER CALL at
identical call counts/callers (operator-new caller rows byte-
equal): the lever's compile-time analysis allocations re-prime
malloc's free lists, and 69's throw-per-iteration allocation
pattern then pays bin walks. The heap-priming artifact class
(the pooled-allocator-rejected / layout-tax family). Mitigation
if ever needed: an arena for the analysis's transients.

⛔ 44_primes_sqrt +3.21%/iter: UNDER-PINNING. The pick pins r12
(11 exit pops); the lever pins nothing callee-saved. The snap
DEMOTES pieces whose interior boundaries sit on non-lin pcs with
zero extension room (a mem-cut flush at a crossed pc; a same-slot
neighbour at zero distance) - while the pick's whole-run pin
needs NO transitions at all and is immune to lin points. Slot 1
qualifies whole-run (no events, evidence, float-free) and got
shredded instead.

⛔ 34_sort +3.88%/iter, 73_multi_unpack +1.81%: OVER-PINNING,
the mirror image. Callee-saved pops: 34 goes 0 (pick) -> 12
(lever); 73 goes 9 -> 16. The scan admits any evidenced used
piece; the pick's >= 3 floor is a COST MODEL for per-ENTRY
overhead (push + entry load + exit flush + pop), and 34/73's
chunks are entered PER CALLBACK CALL - a barely-used pin paid
per entry, millions of times.

THE PICK-PARITY PAIR LANDED 2026-08-23 (inc 4): the admission
floor (run-wide wint >= 3, property F2 watched by name) + the
whole-run rescue (lin-demoted pick-qualified slots merge into one
[begin,end) piece on a run-free register; S3 restated in coverage
form; saw_rescue vacuity watched; the case needed int(runtime())
- a branch's `sc = runtime()` lowers via MoveV = a mem event,
correctly ineligible). MEASURED per-iteration: 34/44/73/86/87 all
+0.00%, 07/43 hold, ⛔ 81 -2.72% and 83 -0.98% - the lever BEATS
the pick on the register-pressure family, the arc's first wins.
Residual s1 band = compile-side + 69's heap class. THE FULL 88-BENCH SWEEP RAN 2026-08-24 (callgrind, -npc both
sides - a first sweep without -npc was DISCARDED and re-run: the
runner passes -npc for exactly this reason, and an ad-hoc script
that bypasses run.py must add it back; 09_fib's non-scaling
denominator was the tell). RESULT: ZERO per-iteration regressions
except 69_exc_crossframe +4.13%, whose emission is BYTE-IDENTICAL
lever-on/off (verified again) - the documented heap-priming class,
scaling with iterations because 69 allocates an exception per
iteration. Six benches over the 0.30% band, five of them WINS:
06_if_branch -16.80% (real allocation: 3 fewer helper calls, 9
fewer tag stores, r14/r15 pinned across the branchy body where
the pick declines), 81 -2.72%, 82 -1.56%, 83 -0.98%, 68_nested
-0.46%. Everything else +-0.00%/iter; the s1 column's +0.0..1.2%
is the compile-side band. The committed spot numbers are confirmed
by the -npc sweep unchanged.
C2b-REGIONS ARC, STEP 1 LANDED 2026-08-24 - THE HAZARD CHECK
ANSWERED AND THE SHARESEAM HOLE CLOSED. The probes settled the
structure (and it is smaller than feared):
 - a corpus scan (MYLANG_SHAREDBG, new) found ZERO programs where
   seams and regions coexist - the hazard was LATENT;
 - IN-region seam pcs were ALREADY impossible: a region lives
   inside a loop, and the loop's back edge makes every interior
   pc a non-lin-point, which seam_ok refuses;
 - ⛔ the ONE reachable hazard pc is exactly L+1: outside the
   loop (uncrossed, lin-LEGAL), yet the cold copy's rejoin jumps
   to label[L+1], which is PAST a seam there - the failed-guard
   edge is an EMISSION construct jit_run_edges cannot see. A
   crafted shape reaches it (early-phase pin freed before the
   loop; a post-loop slot whose lo == L+1, rank-first).
The fix: jit_share_plan's `noseam` ranges ([T, L+1] per region;
T included cheaply); g_jit_share_clamped is the reach counter;
jit_share_region_clamp is the -rt net (WATCHED: removing the
clamp fails only the counter - the value-level demonstration
needs a FAILING C1 guard at runtime, which no lever can force;
that is the designed-not-built cold-arm forcing lane).
THE SAME STRUCTURE ANSWERS THE TMODE QUESTION: transitions inside
a region are already impossible via the snap's lin rule (same
back edge); lifting hregs.empty() from tmode needs only (a) the
snap refusing transition pcs in [T, L+1] per region (mirror of
noseam), and (b) the cold copy emitted against the REPLAYED cache
state at T (base + seams/trans <= T - the stub-replay recompute),
which with (a) equals the state everywhere in the region.
THE TMODE LIFT, ATTEMPTED 2026-08-25 - MACHINERY LANDED, GATES
KEPT CLOSED. What landed and is LIVE: (1) jit_lsra_snap's
`noreach` refusal ((T, L+1] per region - a transition AT T is
legal, emitted before the guard); (2) the cold copies emitted
against the REPLAYED guard-point state (base + ShareSeams <= T +
transitions <= T; snapshot/restore brackets, ra rebuilt busy <=>
entry; no code emitted - the registers hold the values at
runtime). The replay fix is VISIBLE on lever-on 68_nested: its
cold exits re-bound to the guard-point epilogue (jump targets
only; lever-on 68 has seams + regions - ⛔ the coexistence scan
ran DEFAULT config only, lever-on's pin/home economy differs, so
the fresh-window bug was LIVE there in emission, dormant only
because a C1 guard never fails at runtime).
⛔ WHY THE GATES STAYED CLOSED - TWO NEW INTERACTIONS FOUND WHEN
hregs.empty() WAS LIFTED, one diagnosed, one open:
 - DIAGNOSED (structurally, no repro needed): the C2b PAIR's
   DISPLACE branch pops pins from `hot` AFTER the plan spent
   them - under tmode `hot` is the entry-occupant list whose
   ORDER is the abstract-reg zip, so displaced pins stay
   "resident" per the plan but are never loaded (hot_reg is
   built post-displacement, so the aphys ML_CHECK does not even
   abort). The lift must forbid displacement under tmode
   (leftover-only; else drop the second bases - C1 status quo).
 - OPEN, WITH A ONE-COMMAND REPRO: lever-on tmode on
   tests/functional/17_elem2_divmod_roles.my prints mods=1531
   where 513 is right (third value; rt green, corpus catches
   it). The chunk shows NO transitions - the wrongness is the
   tmode PIN SET meeting the elem2 compound-divmod ElemScratch
   ROLES (the file exists to test exactly that class: roles
   moved off preferred registers under pin pressure). Bisected:
   NOT the cold replay (neutralizing it changes nothing). Next
   diagnostic: -vdj diff the divisor-gate sequence between the
   fallback and tmode pin sets on that file's hot chunk.
STEP 3, 2026-08-25 - BOTH BLOCKERS FIXED; A THIRD FOUND BY THE
XROT MATRIX (pre-existing, so the gates stay closed on it alone):
 - the elem2-divmod wrong value DIAGNOSED AND FIXED: RefScratch
   granted the ref-check scratch a register the allocator sees as
   FREE but which holds the op's ISA RESULT (idiv's rdx - the mod
   remainder being stored); the helper then received the type
   pointer's dword as the value. The fix is emit_ref_check's
   `excl` (the VALUE's registers), passed by store_dst and BOTH
   of store_dst_bool's scratches; when even the preferred reg is
   excluded, the push-borrow arm serves (push/pop restores the
   value before use). VERIFIED: 513 under the lifted gate and
   the whole lever-on corpus green with gates lifted. A latent
   hole ANY dense pin set could reach - tmode merely got there
   first. Default emission 116/116 unchanged (rcx free -> the
   grant lands as before).
 - the pair displacement forbidden under tmode (leftover-only;
   the zip-corruption diagnosis stands, commented at the site).
 - ⛔ NEW, PRE-EXISTING (reproduces at HEAD, gates closed):
   MYLANG_JIT_LSRA=1 MYLANG_JIT_XROT=4 on tests/functional/
   16_elem2_fused.my - rotation 4 puts rax first, a lever pin
   lands in rax and SPANS the elem2 fused read, whose raw
   `mov rax, [rcx+r9*8]` never ASKS the Phase-A conflict
   machinery - JIT-REGTRACK aborts at compile (vm pc 67, opcode
   126; the tripwire working as designed). The pick's rax spans
   never crossed that site; the lever's do. ⛔ THE LEVER-ON XROT
   MATRIX HAD NEVER BEEN RUN - add it to the standing battery.
   The fix arc: the elem2 raw-rax sites join the ask-and-evict
   protocol (the Phase-A generic arm), then re-lift the gates
   (they now fail ONLY on this).
STEP 4, 2026-08-25 - THE ELEM2 RAX ASK LANDED: elem_read_plan's
obj role is rax-FIXED (the flat tails' contract) and never went
through pick(), so a pinned rax was written raw; the B2c rule now
applies at entry (reg_pin_conflict -> evict + re-emit with rax
denied; gdb pinned it at e.load(r.obj, base.type), jit.cpp:14200).
Verified: the xrot=4 repro prints correct values; the lever-on
xrot matrix is GREEN on all rotations; rt both configs green.
ALL THREE BLOCKERS ARE NOW FIXED - the RE-LIFT is a two-line
change (drop hregs.empty() from both tmode gates, the exact edit
from step 2/3's diag builds, which passed the full lever-on
corpus + battery when combined with these fixes) plus the
standing battery re-run.
⛔ THE RE-LIFT LANDED AND WAS REVERTED THE SAME DAY (2026-08-25,
f1fe9b0 -> fa52e97) - THE WALL A/B CAUGHT WHAT THE PRE-LIFT IR
SWEEP COULD NOT: 43_sieve +44% wall / +47.4% Ir-per-iteration
lever-on with the gates lifted (the F5 sweep ran pre-lift, when
region chunks still took the fallback; the battery's rt/corpus
are value-blind to allocation quality). The mechanism: tmode's
SUCCESS preempts arm 2, so the facts-based whole-run fallback -
BUILT for 43's idle-prefix counters (+37% lesson) - never runs;
tmode's plan on 43's mem-cut shape serves ~6 residents where the
fallback's pin+home economy serves far more. The defect class:
NOTHING COMPARES THE TWO PLANS. Next increment: a COST GATE -
adopt tmode only when its served weight (residents + homes) >=
the facts fallback's; else decline to arm 2. All three defect
fixes and all the machinery STAY (only the two gate lines
reverted); 43 re-verified at lever-on parity post-revert.
THE WALL A/B RAN (RULE B1 observed: rm -rf build, fresh perf
lane, an env wrapper for the lever side, header checked; 28
excluded - its python-cache recompute times out, the standing
infra item): geomean cur/base 1.001x over 87; wins 85_regs_ref
0.70x, 82 0.81x, 46 0.83x, 18 0.84x, 06 0.92x; the one over-band
loss (43 1.44x) was the lifted config's and is reverted. A clean
wall A/B of the SHIPPING (gates-closed) lever config is still
owed.
THE COST GATE, FIRST CRITERION REFUTED 2026-08-25: the
served-weight comparison (residents+homes vs the facts rule's top
slots) landed - inert while the gates are closed - and a
lifted-gate measurement shows it does NOT decline on 43 (still
+47.4% Ir/iter): 43's inner counters are MEM-CUT, so BOTH sides
exclude them and the weights compare equal. The served-weight
theory of the 43 regression is WRONG. ⛔ NEXT DIAGNOSTIC, before
any new criterion: -vdj diff 43's hot loop between the lifted
tmode emission and the fallback emission - the mechanism is
something tmode emits (per-pc flushes in the loop? the homes-set
difference? textra filtering?) that no weight ledger sees.
THE 43 EMISSION DIFF RAN 2026-08-25 AND NAMES THE DEFECT: in the
hot chunk the fallback pins SIX slots (i, n, j, i2, count, k -
r12..r15, r8, rsi) and tmode pins THREE (n, i, count) - the inner
counters j and k are UNPINNED under tmode. They are MEM-FREE in
this run (the fallback's facts rule passed them) and loop-phase
with an idle prefix - the exact shape THE WHOLE-RUN RESCUE was
built for, and it is NOT FIRING for them. So the 43 regression is
a RESCUE REACH HOLE, not a cost-model gap (which is why the
served-weight gate could never catch it). ⛔ NEXT: instrument WHY
the rescue skips j/k on that chunk - candidates: (a) their pieces
are never resident in the walk (so never lin_demoted - the rescue
triggers ONLY on lin_demoted; a slot whose loop piece the walk
denied a register never enters the set), (b) pass-1 start
relocation "succeeds" to a legal earlier pc so no demotion is
recorded while the piece still under-serves, (c) the free-
register-over-whole-run scan fails against phantom pieces. Repro:
build with both gates lifted (the two-line edit), then
MYLANG_LSRADBG=1 MYLANG_JIT_LSRA=1 on bench/my/43_sieve.my and
read the HOT chunk's pieces (the run whose entry loads name
i/n/j/count/k). Once the rescue reaches them, re-measure 43, then
re-lift; the inert cost gate can then likely be DELETED (its
premise was wrong).
2026-08-25, THE RESCUE THEORY DIES AND THE REAL MECHANISM IS
NAILED: the full piece list shows the scan DOES serve j and k
(pieces [10,21) areg 4 and [22,28) areg 1, installs mid-run; my
"3 pins" read only the entry loads), the binding drops nothing
(probed), and function-level callgrind puts the whole +59.5M
INSIDE the emitted code (123.4M -> 182.9M; every C++ helper
byte-identical). MYLANG_JITSTATS names it in one line:
⛔ two_addr_reg 2,201,542 (fallback) -> 79,496 (tmode) - the
two-address in-place op on a PINNED dst (the 1-instruction form
vs the staged read/op/write through rax) stops engaging, which
is the hot loops' per-iteration tax. Its gate is
`dreg = e.reg_at(in.target) >= 0` at EMIT time (jit.cpp ~15127).
NEXT: instrument WHICH pcs lose reg_at under tmode on 43's hot
chunk (print in.target + pc + reg_at when the two_addr gate
fails but slot_wint says hot) - candidates: the counter ops sit
at pcs OUTSIDE their slots' resident pieces (piece bounds vs the
ops' actual pcs - check [10,21) against the ops using j at
14-18... those ARE inside; so likely ANOTHER chunk or the
i/count slots), or reg_at consults a state the per-pc installs
do not maintain at those pcs. Repro identical to before (lift
gates locally, LSRA_WHY-style print at the two_addr gate).
Curious side signal: elem_reserve 6 -> 3, xcache 1 -> 0.
2026-08-25 REFINEMENT - THE GATE IS NOT THE MECHANISM EITHER:
instrumenting the two_addr emit gate under tmode shows ONE miss
in the whole program (pc 0, slot 4), so the two-address form IS
emitted at the hot pcs both ways - yet its bump EXECUTES 2.2M
(fallback) vs 79K (tmode). Same emission, different RUNTIME
PATH. ⛔ THE COLD-COPY HYPOTHESIS, fitting every observation:
if the C1 guard FAILS per iteration under tmode, the hot loops
run in the REGION COLD COPIES - still emitted native code
(helpers stay identical, jit-region Ir rises, the main stream's
loop bodies measure unchanged) - and the cold copies were
emitted against the replayed state, plausibly without the pins
(staged forms, no two_addr bumps). `hoist` is 170 in BOTH
configs (preheaders run equally), but guard FAILURES are
uncounted. NEXT, the one-command discriminator: a TESTS bump at
each region cold-copy head (emitted identically in both
configs), diff fb/tm on 43 - millions vs ~0 confirms; then ask
WHY the guard fails under tmode (the preheader's r10/r11
establishment vs the per-pc pins, or the replayed-state cold
emission perturbing what the guard compares).
2026-08-25, THE DISCRIMINATOR RAN AND REFUTED THE COLD-COPY
THEORY TOO: g_jit_cold_copy (the new per-verdict counter beside
g_jit_hoist's per-preheader count - a permanent observability
gain, landed) reads ZERO in both configs on 43. The guard never
fails. State of the hunt, all evidence firm: emission
near-identical (1 two_addr gate miss program-wide), helpers
byte-identical, hoist entries equal, NO cold copies - yet the
two_addr bump executes 2.2M vs 79K and the jit-region Ir is
+59.5M. The per-instr callgrind format is now decoded (cost
lines are `pos line cost`, three fields; positions are relative
to compressed ob=/fn= sections - the vdj join needs the ob=
relocation handled). ⛔ NEXT PROBES, cheapest first: (1)
MYLANG_JIT_MAXPINS sweep under tmode on 43 - if the Ir stays
high at EVERY pin count, pins are not the mechanism at all; (2)
finish the ob=-aware join and name the hot instructions
directly; (3) the lever-off matrix (MYLANG_JIT_OFF per lever)
under tmode vs fallback on 43 to isolate which lever's
interaction burns.
PROBE 1 RAN 2026-08-25 AND IS DECISIVE: at MYLANG_JIT_MAXPINS=0
the two configs are EQUAL on 43 (85.3M vs 85.5M, the compile
band). The triangle: fallback pins 73.0M (pins HELP, -12M vs no
pins); tmode pins 92.1M (pins HURT, +7M WORSE THAN NO PINS AT
ALL). So tmode's pin SERVING on this shape costs more than it
saves - consistent with the two_addr evidence if the hot ops
under tmode take a non-two_addr path their pins should enable
(⛔ check `fa` first: the two_addr gate requires !fa, and my miss
print required dreg < 0, so a ferrying/forwarding arm engaging
under tmode at the hot ops was INVISIBLE to the instrumentation
- print the fa-taken case next). A MAXPINS sweep (1..N) under
tmode would show where the hurt starts.
⛔ SOLVED AND RE-LIFTED 2026-08-25/26. The fa print (fa=0
everywhere) narrowed the field to FIVE two_addr sites, ONE
missing (pc 17, slot 4 = j, dreg=-1 in the MAIN stream while its
COLD COPY printed dreg=10 = r10): the aphys binding gave j's areg
r10 at BIND time (region not yet entered), the region's B3 claim
took r10 at its entry, and the install's take_fixed failure was
the seam arm's documented "skipping is safe" - sound for VALUES,
silent abandonment of the promised piece: the hot op ran STAGED
2.2M times, tmode's pins worse than none. THE FIX: a failed
install DECLARES the conflict (pin_conflicts bit -> the pass
re-emits with the register denied, the binding picks another).
43 lever-on lifted: 42.5M Ir/iter = EXACT fallback parity
(two_addr_reg back to 2,201,542; rax_retries 2 = the retry
working). 46's win retained (-5.5%/iter). The REFUTED cost gate
is DELETED (its premise was wrong; the real defect was reach,
not economy). GATES LIFTED FOR REAL: -rt + corpus both configs,
the lever-on xrot matrix, off-arena, census, default vdjcmp
116/116 - all green. ⛔ A PROCESS NOTE that cost one wasted
measurement: an edit verified green can be LOST by a later
git-restore in the same probe session - the "no effect" run was
of a binary WITHOUT the fix; grep the anchor before measuring.
THE CLEAN WALL A/B RAN 2026-08-26 (RULE B1: rm -rf build, fresh
perf lane opt1/asserts0, explicit --mylang/--baseline, header
checked; 28 excluded - the standing cache-infra item; env
wrapper for the lever side, interleaved so drift cancels):
⛔ GEOMEAN cur/base 0.997x OVER 87 - THE LEVER IS A NET WALL WIN
SUITE-WIDE, the arc's first sub-1.0 full-suite verdict. Wins:
85_regs_ref 0.73x, 46_matrix 0.82x, 82 0.83x, 18 0.85x, 83
0.91x, 84 0.91x, 06 0.93x. 43 is GONE from the tail (the install
-conflict fix held on the wall). The tail (59 1.08x, 52 1.07x, a
1.06x band) was Ir-SPOT-CHECKED and is EXACTLY identical per
iteration both configs (59: 24,000,000 = 24,000,000; 52: 27,000
= 27,000) - single-run wall noise on sub-100ms benches, per the
documented small-bench divergence.
⛔ D4 EXECUTED 2026-08-26 - THE MAINTAINER FLIPPED THE DEFAULT
("flip the lever on by default, by somehow leave the lever
togglable (opt-out) in case we need that for debugging"):
g_jit_lsra defaults true; MYLANG_JIT_LSRA=0 is the opt-out
restoring the pick end to end. Battery: -rt + corpus in BOTH
directions, the xrot matrix on the new default, both arenas in
both directions, rel-t TESTS=1 OPT=1 both directions, census -
all green. Default emission changes by design; byte-identity
retires as the default oracle.
#103 RESIDUALS, FIRST LIFT (2026-08-26): the v1 "one machinery
per slot" exclusions are GONE - a transitioned slot keeps its C3
type elision (GP) and its C4a-i read elision + float type elision
(xmm). The deferred review, now done: the seam's evict arm writes
the tag itself, the install loads payload only, and both
elisions' soundness is a property of the SLOT's op set,
orthogonal to residency; a doubly-covered slot at a barrier flush
gets its tag written twice, harmless. MEASURED Ir-NEUTRAL on the
spot set (81/83/06/43/55 all at pre-lift values): the corpus has
no slot that is both elided and transitioned today, so the
filters were filtering nothing - the win materializes only when
that overlap appears, and the bound now cannot cost it. Battery:
rt + corpus both directions, xrot on the default, nolowmem - all
green. ⛔ THE OTHER TWO v1 BOUNDS STAY, EACH WITH ITS WRITTEN REASON
(the guard-elision family's "do not push further" shape):
 - TEMP PIECES: lever A's contract bakes "TEMPS only... a local
   is observable state and may be N5-cached; A TEMP IS NEITHER" -
   skip_write deliberately leaves a dead temp's SLOT stale, and
   every fwd path assumes no register caches it; a pinned temp
   would read stale state through reg_at. Lifting means
   redesigning the forwarding contract (fwd consulting reg_at,
   pin updates on skip, the staleness invariant rewritten) - and
   the prize is backwards: forwarding serves the dominant
   adjacent-temp shape in ZERO instructions, which a pin cannot
   beat; multi-use long-lived temps are rare by construction
   ("codegen's expression temps are consumed exactly once").
 - THE SECOND CHANCE: BUILT, WATCHED, AND REJECTED ON MEASUREMENT
   (2026-08-26). The trimmed retry (a pressure loser - newcomer or
   split remainder, reg -1 non-forced - re-admitted from the
   earliest pc a register is free through its end, span-accurate
   against ALL pieces after the walk-end-state version handed out
   an overlapping register and the snap's model caught it; per-
   piece evidence re-checked on the trimmed range; the firing
   shape needs the WINNER TO DIE before the loser's int tail - a
   winner idling across it blocks the register, and a boxed print
   tail is a mem event, not evidence). It fired, the suite was
   green everywhere including the off-arena lane (after
   jit_hoist_c1 joined the force-the-pick convention, kept) - and
   ⛔ 83_regs_int_40 measured +7.9%/iter (435M vs 403M): the
   marginal residency's install/flush seams cost more than the
   memory it replaced, the admission-floor lesson in a new coat.
   REVERTED per the flatness mandate; a re-attempt starts from a
   PROFITABILITY model for the trimmed range (uses saved vs seam
   cost), not from reach.
 - HOME/PIECE MIXING: the seam flush arms write the FRAME slot;
   a home's stack qword would go stale unless every seam, exit
   and bracket becomes per-pc dual-location-aware - a coherence
   redesign for a shape the whole-run rescue and the homes tier
   already mostly serve (a slot heavy enough to want both is
   heavy enough to be rescued whole-run).
#103 CONV = 0 - THE RATCHET IS CLOSED (2026-08-25). The "3-use
allocatable-roles batch" did not exist: elem_read_plan has
allocated data AND count, and elem_scratch_plan data, since
#96 (c) (2026-08-20) - preferred-first picks over ELEM_CAND with
the pin-conflict eviction on exhaustion, pressure-covered by the
xrot matrix (which found the rotation-4 rax bug in this very
plan). The three tagged literals were pick()'s PREFERRED SEEDS,
identical in role to the idx/val seeds batch 3 already classified
proto. What made them look like open work was a STALE struct
comment ("this lands INERT, and making them allocatable is the
next step") - written when the plan landed inert, never updated
when the picks went live. The comment is fixed IN the same change,
and the lesson is the doc-sync rule's oldest shape: a plan-status
sentence inside a code comment rots the day the next commit lands;
the census priced an already-built conversion because of it.
FINAL LEDGER of the 499: isa 71 / abi 173 / proto 497 / conv 0 -
every use read, every disposition on its line.
#103 BATCH 3 - EVERY SITE READ, CONV = 3 (2026-08-25): the
model-check's CL-shift assert and ElemScratch's count are the ISA
itself (reg:isa; count's comment already said "idiv-fixed"); the
allocated elem roles' default SEEDS (idx/val/obj - the plan
overrides them), the ELEM_CAND pool definition, the R9 ctx-chain
pairing preference and the switch tbl's ask+evict fallback are
protocol. THE 3 USES THAT REMAIN are the genuine conversions,
named by their own struct comments as "the next step": ElemRead's
data/count and ElemScratch's data - making those roles allocatable
is an emission-changing batch with its own Ir ledger, priced for
the maintainer to fund.
#103 THE 38-USE WORKLIST, READ SITE BY SITE (2026-08-25, batch
2): 15 more uses promoted - every one turned out to be the
ask-fallback bracket AGAIN (emit_op/emit_branch's hold() asks
alloc_scratch first and RCX is its refusal path; ref_check's scr
default feeds RefScratch, itself the ask bracket; div_magic's own
comment says "ask-first, like RefScratch's ctor"; store_dst passes
the same scr). CONV 23 remains: the elem base-gate/bounds roles,
the R9 ctx-chain preference, the model-check probes, the switch
tbl - each to be READ before moved; blanket conversion was the
wrong frame for this residue, since the sweep's mechanical
conversions (8b/8c) already put the ask in front of nearly every
site and left only its fallback registers behind.
#103 PHASE E EXECUTED ON THE FIVE FAMILIES (2026-08-25): 461 of
the 499 conv uses are PROMOTED to the terminal `reg:proto` tag -
the phase-E rule's own outcome ("conv must shrink to genuine
fragment protocol ... or convert"): the rax value-conveyance ABI,
the tag-grant identity, the call/ctx-chain protocol, the counter
bump's zero-footprint bracket, and an alloc_scratch ask's push/pop
FALLBACK register (a bracket the ask itself needs). The census
reports proto beside isa/abi (and the (fn)-region form now counts
hits under the REGION'S OWN tag - they were hardwired to conv, so
an abi(fn) region miscounted). CONV = 38 is now the honestly
convertible worklist, priced in the floor file's comment: the
per-emitter scratch DEFAULTS, the elem base-gate roles, the R9
ctx-chain preference, the model-check probes. Comment-only in
jit.cpp - emission untouched (vdjcmp self 108/108, -rt green,
gate ratcheted 499 -> 38).
#103 THE reg:conv SWEEP, INSTRUMENTED AND MAPPED (2026-08-26).
The census now splits justified by tag (isa 69 / abi 173 / CONV
499) and the gate ratchets CONV downward (regcensus_floor.txt's
CONV row). ⛔ THE MAP - the residue is FIVE STRUCTURAL FAMILIES,
not mechanical conversions (those - the xmm staging pair, the
element roles, RefScratch - are already done, which is why 499
uses remain):
 1. THE RAX ACCUMULATOR (acc_take, lever A's conveyance, the
    exit-pc protocol): the fragment's value ABI. Converting =
    redesigning value conveyance; the Phase-A conflict machinery
    already lets rax be pinned around it. HIGH effort, LOW
    payoff post-D4.
 2. THE TAG GRANT IDENTITY (tag_int_reg = RSI, tag_float_reg =
    R8): AUDITED 2026-08-26 - every functional consumer already
    reads the recorded grant (tag_holder / the variables; the
    remaining rsi/r8 literals are COMMENTS), so the plumbing for
    a dynamic pick exists. ⛔ BUT the payoff is OFF-ARENA ONLY
    (the arena default makes tags imm32 - no grant at all), and
    a better pick needs the grant/pick PIPELINE REORDERED (the
    clobber mask consumes the grant's answer before the pick
    runs - circular), while the plausible alternates (rdi, rdx,
    rcx) are all hotter than rsi/r8, which is why B1 chose
    these. Verdict: an off-arena optimization, priced at a
    pipeline reorder for +1 pool member on float runs - NOT the
    first target after all; fund only if off-arena perf ever
    matters.
 3. THE CALL PROTOCOL (the M5b/c record push, ctx chain via r9,
    frame sizes via rsi): a fragment-internal ABI shared by
    every call site and entry stub. Converting = a protocol
    redesign. HIGH effort; payoff only if the protocol registers
    contend, which the clobber mask already prices in.
 4. THE CTX/CAPTURE WALKERS (r9 chains): same protocol family.
 5. bump_counter's rax shuttle: TESTS-only emission - zero
    shipping payoff; convert only for the ratchet's sake.
#103 THE FLOAT-PRESSURE BENCH LANDED (2026-08-26):
bench/my/89_regs_float_08.my + the cpp twin - PHASED float
pressure (two loops, each hot on 4 accumulators + its float
index: 5 candidates per phase vs the pool's 4, 10 across the
run). Two design rules baked into it: the INT loop counters stay
out of every float expression (one usef touch would disqualify
them from the int pool - the disjoint rule), and every constant
is dyadic with identical IEEE op order on both sides (the
4-digit result compares exactly; verified my == tw == cpp).
THE 89 DIAGNOSIS IS COMPLETE (2026-08-24), and the mechanism is
one slot: fj. The emission diff (back-edge-delimited loop bodies;
vm-pc attribution is unavailable post-delete-originals - every op
mark reads pc 0) says loop 1 is IDENTICAL (39 = 39 instrs) and
loop 2 is 48 vs 43 - the whole +5 Ir/iter. In the opt-out body fj
lives in xmm6 all iteration (the PICK pinned it); in the lever
body fj is memory: a tagged store (tag + payload) plus four
reloads per iteration. WHY THE LEVER LEAVES fj UNSERVED: every
float's interval spans prologue-def -> sum-read, so at fj's walk
arrival (pc 9) the pool holds f0/f1/f2/fi and fj is the FURTHEST
next use (loop 2 is ~18 pcs away) - the newcomer stays memory for
its whole [9,51) interval. The pick's static COUNT ranking is a
DENSITY ranking, and it is right here: fi and fj have ~5 uses per
iteration (4 reads + the increment) vs the accumulators' 2, so
the pick pins both indices and wins. f3's whole-run demotion is
the same distance-vs-density miss at pc 8 (fi's arrival evicts
f3, whose 2-op/iter recurrence out-densities fi's 1).
(a) IS BUILT AND MEASURED (2026-08-24): the contest is density
(uses-remaining / span-remaining, cross-multiplied, ties keep the
old smaller-slot direction). Corpus old-on vs new-on: 86 of 88
byte-identical, 89 -6.10% (EXACT pick parity, off=new-on to the
Ir), 68_nested -0.12%, zero regressions; 89 wall 0.90x. Pinned by
the `density beats distance` K=1 case, watched failing under a
next-use sabotage. (b) remains the open follow-up below.
(b) IS BUILT AND MEASURED TOO (2026-08-25, the day after (a)):
the LIFETIME-HOLE pass - see docs/jit-optimizations.md's #103b
entry for the mechanism and the full ledger. The headline pair:
89's plan reaches the phased ideal (drefs -40%, Ir flat, corpus
86/88 byte-identical + two small wins), and chasing its wall
paradox (drefs -40% yet 1.43x WORSE) found a PRE-EXISTING
corpus-wide tax: fmov_rr emitted reg-reg MOVSD, whose upper-half
merge is a false dependency serializing every float chain -
MOVAPS fixed it for 04_float_arith 0.53x, 55_float_sum 0.85x, 89
0.87x wall. The second-chance re-queue LANDED 2026-08-25
(maintainer-directed completion of (b) to its original two-half
design) - the event-driven walk, park/re-bid at lin points, the
density contest as the churn guard; see docs/jit-optimizations.md
#103b-2. 83 - the bench that killed v1 at +7.9% - takes 2 re-bids
at zero per-iteration cost. The 28_str_concat blocker is FIXED
(the .py's module-level accumulator defeated CPython's in-place
append - function-wrapped, 2-11 min -> 0.11s; recompute default
timeout 600s), so the suite-geomean close is unblocked - AND RUN
(2026-08-25, interleaved --baseline at 03b9d09 = density-only):
cur/base geomean 0.972x over 89 benches - the movaps + holes +
re-queue stack is a ~3% suite-wide wall win, led by the float
benches (04_float_arith 0.52x); my/python geomean 12.12x.
THE FIX CLASS, for the maintainer to sequence: (a) DENSITY-AWARE
eviction/admission (uses-remaining / span-remaining instead of
next-use distance) reaches roughly pick parity on this shape; (b)
INTERVAL SPLITTING WITH LIFETIME HOLES (split fj to a hot [25,40)
piece AND split the phase-1 pins' idle gaps so a register is free
to receive it) beats both - each phase would pay ONE memory slot
where the pick pays two + four. (b) is the second-chance
direction, rejected once on 83's +7.9%; 89 is now the
counter-bench that motivates its careful revival with the idle-
gap half built too.
FOUND WHILE DIAGNOSING, FIXED FIRST (infrastructure rule): the
disassembler printed a REG-FORM movsd/sqrtsd rm operand with the
GP table ('movsd xmm0, rsi' for movsd xmm0, xmm6) - the 0x58
arithmetic family had the reg-form fix, the 0x10/0x11/0x51 arms
did not. disasmcheck.py is structurally blind to it (it compares
MNEMONICS, not operand text) - recorded as a known oracle limit.
Also: jit_lsra_assign's float-mode LSRADBG now dumps fpiece rows
(only ftrans was printed - the phase-2 diagnosis was impossible
without the pieces).
⛔ THE ORACLE'S FIRST VERDICT: THE LEVER LOSES THE SHAPE -
+6.5%/iter lever-on (164.0M vs 154.0M), even though the float
machinery visibly engages (lsra_fpins 1, lsra_ftrans 8 = the
phased ideal's 4 evicts + 4 installs). The per-pc float pieces
UNDERPERFORM the pick's whole-run 4-of-10 on the exact shape
they were built for. NEXT DIAGNOSTIC (the 43 mold): LSRADBG the
plan on 89's work chunk - are the phase-2 pieces served,
demoted, or rescued into something degenerate; then the -vdj
loop-body diff lever on/off. Repro: the bench at scale 1/3,
callgrind, MYLANG_JIT_LSRA=0 as the baseline.
THE FLIP GATE WAS - THE MAINTAINER'S CALL, with the
ledger now complete on both axes: Ir (zero per-iteration
regressions corpus-wide; wins 06 -16.8%, 81 -2.7%, 82 -1.6%, 83
-1.0%, 68 -0.5%, 46 -5.5% post-lift, 43 at exact parity) and
WALL (geomean 0.997x, wins to -27%, tail verified noise). The
float twin is complete and flat; the gates are lifted; every
watched net is green in both configs, both arenas, every
rotation. Compile-side cost: the +0.0..1.2% s1 band (task #102's
RTTI work is the remedy). D4 (default ON) would retire the pick
after a soak; the lever-off config would then need the same
coverage discipline the lever-on one got.]

⛔ IN PROGRESS (2026-08-24): THE FLOAT/XMM TWIN - the maintainer's
"continue with the float/xmm twin". The mandate's xmm half: the
scan allocates the FLOAT pool (today the lever leaves fhot to the
pick in BOTH modes). Current float machinery: C2a pins xmm4-7
(FCACHE_REGS, MAX_FCACHED=4, all caller-saved - no entry push;
emit_call_prologue/epilogue spill/reload e.fcache around helper
calls, reload_cache re-seeds after barriers); the pick's rule is
local + !disq_f + uses >= 3 + fdst (written by a float op in the
run).

⛔ THE ONE REAL ASYMMETRY vs the int side, and the rule it forces:
A USEF READ IS NOT TYPE EVIDENCE. A float op can legitimately READ
a definitely-int slot through the promote arm, so admission
evidence is a float WRITE, never a read - a float pin's entry
movsd would otherwise reinterpret an int payload as a double (the
pick's own soundness note at the C2a accounting, ~jit.cpp:11019).
Per-piece translation (the d1 lesson applied to floats): a piece
is admissible only when it contains an fdst event AND no usef read
PRECEDES the first fdst in the piece - the pre-write window's
entry-loaded garbage is then dead (def-before-use inside the
piece), and everything after the first write is float (a non-float
writer would have disqualified/cut). Cross-piece "the previous
flush proved t_float" reasoning is deliberately NOT used -
conservative costs opportunity, never soundness.

THE F-LADDER (mirror of the int increments, same lever):
 F1. LANDED 2026-08-24: FltEvent {pc, slot, kind: read|write|mem}
     stream from jit_qualify_intervals; SELF-CONTAINED for cutting
     (mem entries from BOTH bad() and badf(), so the float scan
     never merges with the GP stream, whose badi() must not cut
     it). Property E-float (stream reconciles with uses_float/
     wrote_float/mem_float per interval) + 3 vacuity guards +
     the MoveV-dest cut-event case. WATCHED: mislabeling a usef
     read as a write fails the suite. ⛔ FINDING: a badf-ONLY site
     does not exist - every visitor badf() is paired with bad()
     (MoveV dest, CmpFloatV dst) - so badf's own push is marked
     DEFENSIVE (9301c45 convention), structurally unwatchable
     today.
 F2. LANDED 2026-08-24: jit_lsra_assign gains float mode (a
     trailing `fev` param; non-null flips the pool). Cuts,
     evidence, floor and split-worthiness all derive from the
     FltEvent stream; disq = interval uses_int > 0 (uses_ret
     exempt); evidence = write-first per piece (fw != MAX and no
     read strictly earlier); floor = run-wide read-weight >= 3.
     jit_lsra_float_check: FI1 tiling, FG write-first (WATCHED:
     accepting read-first names the piece), FD disjoint pools,
     FF2 floor (WATCHED: inflating the weights names the slots),
     4 vacuity shapes. ⛔ TWO FINDINGS: LoadImmFloat is a FLOAT
     WRITE (usef+fdst, badi only) - so an uncut float accumulator
     is write-first FROM ITS DEF and legitimately resident; the
     read-first refusal therefore needs a float-side CUT between
     def and loop (a DictStore key bad()), and its vacuity
     detector must run PER PIECE - an interval's early def write
     hides read-first at interval granularity.
 F3. LANDED 2026-08-24: jit_lsra_snap's trailing `fev` param
     flips the RESCUE eligibility to the pick's fhot rule (sum
     uses_float >= 3, wrote_float somewhere, no mem_float, no int
     uses); everything else shared verbatim. jit_lsra_snap_float_
     check: FS2 replay, FS5 single-residency, the float rescue
     shape (`sc = float(runtime(7))` in a branch - a builtin DST
     is a BARRIER, not a bad(), so sc stays pick-eligible while
     the branch edge demotes its piece boundary). WATCHED: forcing
     fm-ineligibility trips the rescue vacuity guard.
 F4a. LANDED 2026-08-24: lever-on fhot from the interval FACTS
     (the pick's rule restated - never the plan's pieces, the
     43_sieve idle-prefix lesson). EXACT PICK PARITY MEASURED:
     lever-on vdjcmp 116/116 byte-identical (OPT=1 ASSERTS=0 -
     ⛔ a TESTS build's counter bump perturbs the dump, so parity
     vdjcmp needs counters compiled out). Default 116/116.
     g_jit_lsra_fpins execution proof, WATCHED via the bridge
     check (skip the replacement -> counter flat).
 F4b. CORE LANDED 2026-08-24: float trans mode - per-pc xmm
     pieces execute through e.fcache. The bridge runs the F2/F3
     scan+snap (fev mode, same v1 bounds: no temps, hregs empty;
     a transitioned slot leaves textra_f AND fread_raw - one
     machinery per slot); fhot becomes the entry-occupant list;
     afphys binds abstract regs to FCACHE_REGS (⛔ TWO loops -
     take all, THEN give back the occupant-less: an fgive inside
     the take loop handed the SAME xmm to the next areg, watched
     aborting `c.slot == tr.evict_slot` on 01_float_chain);
     seam arms fstore+t_float-tag / fload with fgive/ftake_fixed
     per pc; entry stubs replay base_fcache + transitions (the
     inc-2 final-state lesson, float file). Verified: -rt +
     corpus both configs, off-arena lever config, default vdjcmp
     116/116. Real float transitions execute on the corpus
     (01_float_chain: install@9/11, evict@22/28 across 2 runs).
     F4b CLOSED 2026-08-24: g_jit_lsra_ftrans (a JITSTATS row)
     bumped from the emitted float seam only; the bridge check
     runs the 01_float_chain shape and requires it to move.
     WATCHED: declining float-tmode adoption leaves 1953/1954
     green - every value right via the F4a fallback - and fails
     ONLY the counter assertion. REACH (JITSTATS, lever on,
     -npc): 04_float_arith 2 ftrans/run, 55_float_sum 4; 54 and
     88 enter with lsra fpins and no transitions; 46/79 decline
     (their float slots are mem-cut or int-mixed). Lever-on
     emission now legitimately DIFFERS from the pick on float
     shapes - the oracle from here is corpus_diff (green, both
     arenas) + F5's per-iteration Ir, not byte identity.
 F5. CLOSED 2026-08-24: the 8-bench float spot set is +0.00%/
     iter everywhere with the twin fully active (04 executing 2
     float transitions per run, 55 four), and the FULL 88-bench
     sweep at HEAD reproduces the pre-twin ledger EXACTLY - the
     same 5 wins (06 -16.80, 81 -2.72, 82 -1.56, 83 -0.98, 68
     -0.46), 69's heap artifact, everything else +-0.00%/iter.
     THE FLOAT TWIN IS COMPLETE AND FLAT. Honest gap recorded:
     no WINS from float transitions yet - the corpus has no float
     register-pressure shape (the regs family is int-only); the
     transitions run entry-adjacent on today's benches, invisible
     per iteration. A float regs-family bench (5+ hot float
     locals, phased) is the shape that would show the payoff, a
     candidate when the arc returns to measurement.]

⛔ MAINTAINER DIRECTION (2026-08-23): #96 was PAUSED for the #99
DETOUR; #99 closed same day. Task #102 created for the
RTTI compile bill (his call: "optimize the compilation step
itself"). #99 progress lives in the TASK's own description
(67_make_dict: attribution confirmed at 202f816, tier intact,
~19-27 Ir/callback inside the unified entry - next is the
builtin_make_dict diff and the argv-borrowing-overload-vs-
accepted-trade decision; 09_fib: the #93-window -vdj diff not
yet started). Bench-infra breakages to fix en route: 28's python
timeout, 78's timing failure.
ON RESUME (#96, 2b-iii-d remainder): 81's +1.24% (lever-only,
ships nothing), second chance, C2b regions, float twin, arm 3 if
exits stop flushing wholesale, the flip gate (D4 green AND the
ledger pays).]

### D2. The per-pc assignment seam  [LANDED 2026-08-23]
RESULT: reg_at/spill_at/freg_at (Emitter, beside creg/cspill/fcreg)
answer "where does slot s live AT cur_pc?" - dbg_pc was PROMOTED to
cur_pc, model state both emission loops maintain per op. All 37
query sites (20 creg + 11 cspill + 6 fcreg) migrated; vdjcmp
116/116 both arenas, -rt 1948/1948. creg/cspill/fcreg are now
INTERNALS of the assignment: the wholesale machinery (flush_cache/
snapshot/restore/clear, emit_call_prologue/epilogue, entry stubs,
exit_pc) still iterates the vectors directly, which is correct
while the assignment is whole-run - D3 makes those "the assignment
AS OF THIS PC" (RetFlush's restated contract) and rebuilds entry
stubs from the per-pc map.
 - DEFERRED TO D3, with the reason recorded: "the tracker learns the
   map" is only definable once the map exists as an artifact
   SEPARATE from the wrapper (D3's assignment output) - checking the
   wrapper against itself today is the oracle-shares-its-subject
   trap verbatim.

### D3. Linear scan with splitting (the brain)

DESIGN DECISIONS SETTLED 2026-08-23 (build from these):
 - BYTE-IDENTITY ENDS HERE, BY PLAN: the allocator goes behind a NEW
   LEVER (`lsra`, default OFF) - all nets stay green trivially while
   it is built; dedicated runs enable it; the default flips only when
   the full D4 net is green AND the D0 ledger says it pays. A
   lever-off config keeps today's pick path byte-for-byte.
 - QUALIFICATION GOES PER-INTERVAL: the pick's "EVERY use in the run
   is cache-aware" becomes "every use INSIDE THIS INTERVAL is" - the
   payoff shape: one late boxed op no longer costs a slot its whole
   run. Step 1 of the integration is a BYTE-IDENTICAL refactor:
   export the pick's bad()-switch as a per-(op, slot) classifier
   {none, cacheaware, memory-demanding}, consumed by the pick exactly
   as today, so the interval side and the pick cannot drift.
 - A MEMORY-DEMANDING op (&slot takers, the g_current_ctx readers) is
   a FORCED INTERVAL END at that pc - the structural constraint that
   retires the bad() list (its own measured note says the flush-
   around trade does not pay, so ending the interval is right).
 - COLD-ARM HELPER CALLS NEED NOTHING NEW: emit_call_prologue/
   epilogue's spill-around-call discipline is ASSIGNMENT-AGNOSTIC
   (it iterates whatever is currently assigned) - it already covers
   any caller-saved assignment, which is what makes per-interval
   caller-saved assignments safe from day one.
 - LABEL RESOLUTION v1: a canonical per-label assignment with fixup
   moves on in-edges; the LOOP HEAD (reached from above AND the back
   edge) is the case that matters. Block-local-only allocation was
   considered and REJECTED: it loses the loop-carried pin, which is
   the N5 win the whole model exists for.
 - VALIDATOR ARMS 2/3 (label agreement, exit-flush completeness)
   land WITH the lever, before the default flips.
 - Walk intervals by start; free-until/next-use-distance; at
   pressure evict/split the interval with the furthest next use;
   SPLIT before helper calls for caller-saved assignments (callee-
   saved intervals may span them - the existing spill-bracket
   discipline retires); spill to the native-stack homes;
   REMATERIALIZE LoadImmInt-defined values instead of spilling.
 - Moves at split points; at LABELS (loop heads, branch joins) the
   assignment must AGREE along every in-edge - v1: a canonical
   per-label assignment with fixup moves on in-edges (the classic
   resolution pass). The native back edge is the case that matters
   (loop head reached from above AND from the back edge).
 - COST MODEL: gp_weight seeds it; callee-saved preferred across
   calls, caller-saved in call-free spans; rax's ABI/encoding
   advantages live HERE (the maintainer's rule: facts in the model,
   not per-site requests).
 - The bad()-rules retire into structural constraints: an op that
   takes a slot's ADDRESS (lea rdi, slot; the boxed 24-byte forms)
   forces the interval into memory at that pc - derived from the
   emitters' own asks, not from an opcode list.
 - C3 type-elision and the xcache clobber masks retire likewise
   (a helper call is a split point; a singleton grant is an
   interval).

### D4. The oracle strategy for a change that CANNOT be
### byte-identical
 - Correctness: the 5-mode differential, corpus plain/--levers/
   --xrot/--cold/--nolowmem, all four fuzzers, Net 2/3, rel-hard,
   clang, MSVC via CI - the full net, because vdjcmp stops being an
   equality oracle the day the allocator changes emission.
 - vdjcmp still self-tests (a binary vs ITSELF must stay 108/108) -
   it remains the reproducibility oracle, not the change oracle.
 - disasmcheck (objdump) still proves the dump decodes - run it on
   the new emission.
 - A NEW static validator (the verify_chunk philosophy, at emit
   time): every emitted read of a slot matches the assignment at
   that pc; every label's in-edge assignments agree; every exit
   flushes exactly the live-and-dirty set at its pc. This is the
   replacement for byte-identity - it must FAIL LOUDLY on a wrong
   allocator, and be watched failing (sabotage: skip one fixup
   move).
   [ARM 1 LANDED 2026-08-23: slot_mem_check - every [rbx+disp]
   PAYLOAD access through load/store/fload/fstore/cvt aborts if the
   assignment at cur_pc homes the slot in a register or spill (the
   value is elsewhere); gates mirror wrote() (machinery/flushed/
   bracket). Its FIRST RUN found the call prologue's pin spills
   undeclared as machinery - fixed at the source with PinMach, the
   same declaration the epilogue's reloads carry. WATCHED failing:
   a seam-bypassing read_slot aborts by name with the slot number.
   The label-agreement and exit-completeness arms become definable
   with D3's varying assignments.]
 - Performance: RULE B1 full-suite A/B + callgrind on the regs
   family and the top-8 my/cpp benches.

### D5. Retirement
 - pick_cached_slots, the pin-contract bad() tables, MAXPINS/
   MAXSPILL knobs re-expressed against the allocator (the marginal-
   value-of-one-register instrument must survive - re-derive it as
   an allocator budget), JITSTATS rows for split/spill/remat counts.

### D6. #101 (peephole/scheduling) happens AFTER D, on the
### allocator's output - not before, not beside.

---------------------------------------------------------------------
## PHASE E - RETIREMENT + DOC SYNC
 - Delete the legacy prefer masks (cost model owns preference).
 - Re-litigate every reg:conv tag (grep reg:conv = the worklist):
   isa/abi stay; conv shrinks to genuine protocol or converts.
 - regcensus: the justified column should end as isa/abi/protocol
   only; floors stay ZERO for both files.
 - CLAUDE.md's JIT sections + docs/jit-optimizations.md entries for
   every phase; memory files updated.

## RESUME PROTOCOL (post-compaction)
 1. Read this file top to bottom.
 2. `python3 scripts/regcensus.py --gate` and `git log --oneline`
    tell you where the arc stopped.
 3. The phase checklist at the top is the truth; the per-phase
    sections carry the design decisions already made - do not
    re-derive them.
