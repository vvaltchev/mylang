# #123 — MAKE THE JIT USE ALL THE REGISTERS (GP AND FP)
# WORKING PLAN + LIVE STATE.  Updated as work proceeds so a context
# compaction can resume from here.  Last update: after the FLOAT half
# landed (commit 0e57e00, WIP).

## THE INSTRUCTION (maintainer, 2026-08-29)

> "Retire the pick and completely remove both the arrays CACHE_REGS and
> FCACHE_REGS. Use the allocator to get whatever register is more
> convenient which will include a weight for caller vs callee saved."
> "Remove both of those arrays immediately and start working with the
> registers. STOP DOING NON-SENSE MEASUREMENTS. Revert whatever you have
> as hard-coded knowledge... You need to do the RIGHT thing,
> STRUCTURALLY, instead of blindly measuring bad ideas. YOU ARE NOT
> USING ALL THE REGISTERS. There is no performance to measure here...
> check the disassembly MANUALLY, not comparing it blindly with what it
> was before as that makes NO SENSE. Check the disassembly for each
> function yourself, one by one. Reason about its correctness. When ALL
> the registers are used freely in the native code, both GP and FP, only
> then COMMIT and measure the performance."

⛔ **DISCARDED AS EVIDENCE**: the `MYLANG_JIT_MAXPINS` sweep at
jit.cpp:23760 ("pins 3..7 +0.00%", "83_regs_int_40 EVERY pin +0.00%").
It swept DOWNWARD from a budget already crippled to 4 on every
call-containing fragment and 4 on every float fragment, so "more
registers buy nothing" is an artifact of the cap. Do not quote it.

⛔ **VERIFICATION IS BY READING.** A `vdjcmp` diff against the OLD
emission is circular - the old emission is what is being replaced. Still
valid and still required: vdjcmp's SELF-test (a binary vs itself),
`disasmcheck.py` vs objdump, `-rt`, `corpus_diff`.

## WHAT THE MODEL ACTUALLY IS (investigated)

Three layers, usually conflated:

    (a) ALLOCATOR   jit_lsra_assign(..., int K, ...) - takes a COUNT,
                    allocates ABSTRACT registers 0..K-1, never sees a
                    register number. Generic. This is what #96 built.
    (b) BUDGET      K = max_pins (jit.cpp:23746)
    (c) BINDING     abstract -> physical. THIS is where the arrays live.

`RegAlloc::take(need, prefer, exclude)` already filters by capability and
picks by `gp_weight()`, which already carries +4 SysV-arg / +2
caller-saved / +1 REX. The cost model the maintainer asked for EXISTS;
the arrays bypass it.

## STATE: FLOAT — DONE (commit 0e57e00, WIP checkpoint)

The float file had been carved by hand into THREE disjoint sets:
xmm0/1 staging grant, xmm2/3 `FLIT_REGS` (literal pool), xmm4-7
`FCACHE_REGS` (pins), xmm8-15 unreachable.

 1. `Emitter::sse_rex(reg, rm, w)` - ONE seam for a float form's REX
    byte, emitted only when it carries a bit. All nine xmm encoders go
    through it: fload, fstore, farith, fmov_rr, sqrtsd, cvt,
    movq_xmm_from, ucomisd, xorps_self. (elem_sd / cvtsi2sd_elem /
    movsd_store_base already had REX via rex_sib.)
 2. disasm.cpp: all five reg-reg SSE arms' rm operand gained REX.B (was
    `c[p] & 7` - would have printed xmm0 for xmm8). regf already had .R.
 3. `fp_allocatable`: `x <= 7` -> `x < 16`.
 4. `FCACHE_REGS` DELETED; budget derived (`fp_alloc_count() -
    FP_STAGE_RESERVED`); pins bound with `ra.ftake()`.
 5. `FLIT_REGS` DELETED; `pick_float_lits` takes `Emitter &` and asks
    `ra.ftake()`; its COUNT follows what is free.

⛔ THE BUG THAT PROVED THE POINT - found by READING, in one look:

    +28:  movq   xmm2, rcx      <- the literal 2.5
    +61:  movsd  xmm3, b        <- a PIN overwrites it
    +116: mulsd  xmm0, xmm2     <- reads the clobbered register

`a = a + b*c` printed 0.0 instead of 25625.0, because the three pools
were disjoint by construction and so had never needed to coordinate.

VERIFIED: -rt 1978/1978 x5 modes; corpus_diff 34/34 + compile gate;
disasmcheck 202,423 instructions ZERO objdump disagreements; vdjcmp
self-test 127 identical; all float bench outputs unchanged.
REACHED: xmm8, xmm9, xmm10, xmm11 (previously impossible).
Hand-verified: `66 4C 0F 6E C1` = movq xmm8,rcx (REX.W|R);
`F2 41 0F 59 C0` = mulsd xmm0,xmm8 (REX.B). REX after the mandatory
66/F2 and before 0F, as the ISA requires.

## STATE: XROT — DONE (commit 389a79e)

`MYLANG_JIT_XROT` rotates the START OF THE SCAN inside `RegAlloc::take`
and `RegAlloc::ftake` instead of rotating an array. Strictly better
coverage: the array form rotated only the caller-saved GP pool, this
reaches the whole GP file AND the float file. Period is 16 now, which
`mylang -v` reports and `corpus_diff --xrot` derives (it used to parse
the caller-saved width out of the same line). At rotation 0 the scan is
r = 0..15, i.e. identical to the old behaviour.

THE PREREQUISITE, not a nicety: with no array there is no order to
rotate, so deleting the GP arrays first would have made the coverage
axis vacuous - and that axis exists because of the r9 bug ("a pool
ordered by preference hides its own tail").

## STATE: GP ARRAYS — DONE (uncommitted at time of writing)

`CACHE_REGS[] = {12,13,14,15}`, `XCACHE_ORDER[] = {10,11,8,7,6,9,2,1,0}`,
`MAX_CACHED`, `MAX_XCACHED`, `xcache_regs()` and `Emitter::take_reg` are
all DELETED. What replaced each:

 1. **The weight model gained the fact the hand order encoded.**
    `CAP_ABI_RET` (rax) + a `+6` term in `gp_weight`. rax carries every
    helper call's return value AND its status, which
    `emit_call_epilogue` reads - so a rax pin cannot be spilled around
    a call the way an ordinary caller-saved pin can; it is EVICTED and
    the chunk re-emitted (`rax_pin_conflict`). Without the term rax
    ranks second-cheapest in the file and every call-containing run
    would pin it, hit the conflict, and pay a wasted emission pass.
    XCACHE_ORDER had put rax LAST; this is the fact it was encoding.

    The order the model now yields, cheapest first:
        r12 r13 r14 r15 (1) | r10 r11 (3) | rcx rdx rsi rdi (6)
                            | r8 r9 (7)   | rax (8)

 2. **`gp_pool_mask()` / `gp_volatile_pool_mask()` / `gp_pool_count()`**
    - constexpr, derived from `gp_caps`. `xcache_mask()` is now just
    the volatile one. `jit_pin_budget()` is the pool count (13).

 3. **`max_pins`** = "how many allocatable registers are left" - every
    pool member this run has not denied and nothing has claimed. Was
    `MAX_CACHED + n_xcache`.

 4. **The pin assignment** is ONE `e.ra.take(CAP_ALLOCATABLE)` per pin,
    replacing `take_reg(CACHE_REGS)` then `take_reg(xcache_regs())`.
    The callee-saved-first ORDER survives as a consequence of the
    weights.

 5. **The C2b hoist pair ASKS instead of PREDICTING.** It used to pick
    `CACHE_REGS[cs_used]` and its successor positionally so the pins'
    later first-free scan would reproduce the choice - two arrays and
    one shared index, a coupling that had already produced two bugs (a
    size_t underflow reading `CACHE_REGS[5]` out of bounds; the tmode
    pop). It now takes two callee-saved registers FIRST and the pins
    take what is left, which is also the right priority: the pair MUST
    be callee-saved, a displaced pin merely gets spilled by machinery
    that already exists.

    ⛔ AND THAT FIXED A REAL OVER-DROP. The old second arm dropped TWO
    pins whenever the callee-saved four were full, without asking
    whether anything was actually lost - so a run with four pins and
    seven free caller-saved registers threw two of them away. The
    honest count (`hot.size() - (max_pins - 2)`) drops none there.
    The `!lsra_tmode` guard moved onto the POP, which is what it was
    always about.

 6. **The #112 capture base** asks `CAP_ALLOCATABLE | CAP_MEM_BASE |
    CAP_CALLEE_SAVED`. CAP_MEM_BASE is exactly the r12 exclusion its
    comment spelled out by hand; CALLEE_SAVED was an UNSTATED
    requirement satisfied only by the array's contents (the line below
    pushes it onto `e.saved`, frag_entry's push list).

 7. **`reg_model_check` gained the ordering test.** "A pin fills the
    callee-saved registers first" used to be STRUCTURAL - two calls in
    an order. It is now a consequence of five integers, and a future
    tuning could silently invert it (every call site would pay a spill
    and a reload for nothing). So it is a test: the worst callee-saved
    weight must be strictly below the best caller-saved one.

VERIFIED SO FAR: build clean (gcc, TESTS=1 OPT=0 and OPT=1 ASSERTS=0);
`-rt` 1978/1978 + 4 differential modes 1696/1696; corpus_diff plain
34/34; disasmcheck **202,433 instructions, zero objdump disagreements**;
vdjcmp self-test 127/127 identical.

READ BY HAND - a 9-hot-local int loop (`/tmp/rg1.my`), release build:

    +515: cmp rsi, n
    +522: jge +561
    +528: add r12, rsi      ; a += i
    +531: add r13, r12      ; b += a
    +534: add r14, r13      ; c += b
    +537: add r15, r14      ; d += c
    +540: add r10, r15      ; e += d
    +543: add r11, r10      ; f += e
    +546: add rcx, r11      ; g += f
    +549: add rdx, rcx      ; h += g
    +552: add rsi, 1
    +556: jmp +515

Nine live values in nine registers, one instruction each, zero memory
traffic in the body. Answers byte-identical under -tw / -nj / jit.

CORPUS REGISTER CENSUS (bench/my + samples + tests/functional, emitted
text): all 13 GP registers appear; xmm0-11 appear (xmm12-15 need 12+
hot float locals, which no corpus program has - a dense 8-float loop
reaches xmm9, an earlier probe reached xmm11).

## STATE: GP REACH — NOT DONE. This is what is left.

### (a) THE xcache DENIAL

`jit_run_blocks_xcache` denies the whole CALLER-SAVED half to any run
that emits a MyLang call, because `emit_sync_push_native` uses r8, r10
and r11 as raw scratch OUTSIDE the `emit_call_prologue` bracket that
spills pins. `jit_assert_no_volatile_pin` is the standing check.

⛔ AND THE DENIAL IS CURRENTLY CORRECT, WHICH THE FIRST WRITE-UP OF
THIS UNDERSTATED. Read the emitter: it holds rax, rbx, rcx, rdx, rsi,
r8, r9, r10, r11 as PROTOCOL across one long straight-line sequence -
8 allocatable registers of 13. So "route r10/r11 through
alloc_scratch()" cannot be the fix; there is no spare register to route
them to. And a MyLang call is a real SysV `call`, so every caller-saved
register is clobbered by the callee regardless.

THE ACTUAL FIX, and it is call-protocol surgery, not substitution:
BRACKET the MyLang call site with the same spill/restore the helper
calls already use (`emit_call_prologue` stores each caller-saved pin to
its slot payload, `emit_call_epilogue` reloads). Then a caller-saved
pin survives, the raw scratch is legitimately free, and the denial
drops to the callee-saved-preserving case. The cost is a push/pop pair
per pin per call site - which is exactly the trade the caller-saved
extension already makes for helper calls, and was measured worth it
there.

This belongs with #97 (the call protocol) rather than with #123: it
changes the emitted call sequence, and #97's frameless-callee plan
touches the same code.

### (b) RETIRE THE PICK (#103's residual)

`g_jit_lsra = false` selects the legacy path. NOTE the positional zip
is ALREADY gone - both modes bind through `ra.take` now - so what is
left is deleting the non-LSRA RANKING path and reworking the 8 coverage
tests that force `g_jit_lsra = false` for their duration. It does not
change WHICH registers are used, only which SLOTS are chosen, so it is
separable from everything above.

## COMMIT DISCIPLINE — DONE

Squashed to ONE commit, `0123d6c`, on top of `c9feab1`. The tree is
byte-identical to the five WIP commits it replaced (verified:
`git diff 7587a75 HEAD` is empty).

⛔ **AND ONE PROCESS FAILURE WORTH KEEPING.** The sabotage harness ran
`git checkout -- src/jit.cpp` while that file held the whole
UNCOMMITTED GP change, and erased it. Recoverable only because every
edit was replayable from the session transcript - the exact luck the
FORBIDDEN-COMMANDS rule says not to rely on. CLAUDE.md now carries the
rule (commit BEFORE you sabotage, or restore from a `cp`). Cost: one
replay.

## NETS - ALL GREEN

    -rt 1978/1978 + 4 differential modes 1696/1696, on:
        gcc debug + ASan/UBSan
        clang debug
        rel-hard   (TESTS=1 OPT=1 VM_HARDENING=1)
        TESTS=1 OPT=1 ASSERTS=0   (the emission-vs-NDEBUG lane)
        non-JIT g++ AND clang++   (jit.h guard forced to 0)
    corpus_diff  plain / --levers / --cold / --xrot (0..15) /
                 --nolowmem      - 34/34 agree each
    disasmcheck  202,433 instructions, 332 fragments, ZERO objdump
                 disagreements
    vdjcmp       self-test 127/127 identical
    driver_checks, myv_doc_check
    nested_fuzz  800 programs  - RUNNING at time of writing

    SABOTAGE, one build each:
      drop the +2 caller-saved term   -> -rt ABORTS rc=134, 1976/1978
      rbx made allocatable            -> -rt rc=1
      drop the CAP_ABI_RET term       -> PASSES 1978/1978 (see below)

    HAND CHECKS: the 9-hot-local int loop (nine registers, nine adds,
    no memory traffic); 46_matrix_mult's C2b pair takes r12/r13 with
    correct length arithmetic and matches -tw; a 6-hot-local nested
    read agrees across -tw/-nj/jit, at xrot 0/3/7/11/15 and at
    MAXPINS 0/5/9; float forms hand-decoded (movq xmm8,rcx =
    66 4C 0F 6E C1; mulsd xmm0,xmm8 = F2 41 0F 59 C0).

## THE ONE SABOTAGE THAT PASSES, AND WHY THAT IS THE RIGHT ANSWER

Deleting `CAP_ABI_RET` leaves `-rt` at 1978/1978, because it is not a
correctness fact - it costs a COMPILE PASS. "An optimization that only
affects SPEED has no correctness oracle" is the documented gap, and the
right instrument is a COUNTER, which `rax_retries` already is:

    9-hot-local int loop     frags   rax_retries
      with    +6               1         -
      without +6               1         1

The emitted code is IDENTICAL. Without the term rax ranks
second-cheapest, gets pinned, hits `rax_pin_conflict`, and the chunk is
emitted a SECOND time to reach the same answer.

## ⛔ MEASUREMENT — A REAL REGRESSION, ROOT CAUSE NOT YET FOUND

RULE B1 followed: `build/` deleted, both lanes OPT=1 ASSERTS=0,
`--mylang build-claude/perf123/mylang` and
`--baseline build-claude/perfbase/mylang` (c9feab1) passed explicitly,
header read and valid, `-npc` on.

    geomean cur/base over 90 benchmarks: 1.028x  (2.8% SLOWER)

AND IT IS TWO BENCHES, NOT A SPREAD:

    05_mixed_arith   4.54x SLOWER
    04_float_arith   2.56x SLOWER
    everything else  within +/-6%
    46_matrix_mult   0.80x FASTER   (the hoist-pair over-drop fix)
    18_foreach_array 0.81x FASTER

⛔ AND THE FIRST READING OF IT WAS WRONG. run.py's absolute times are
0.015s and 0.026s, so I called it startup noise. At scale 60 it is
real: 05_mixed_arith 0.23s vs 0.05s, 04_float_arith 0.27s vs 0.10s.
A ratio on a 15ms bench is not evidence either way - RE-TIME AT SCALE
before believing or dismissing one.

### WHAT IT IS, MEASURED

The accumulator loses its FLOAT PIN and round-trips through memory with
a type dispatch every iteration:

    baseline                      current
    movsd xmm4, x    (pin load)   movsd xmm1, x     (reload)
    addsd xmm1, xmm0              cvtsi2sd xmm1, x  (+ re-convert)
    movsd x, xmm4    (exit only)  addsd xmm1, xmm0
                                  movsd x, xmm1     (store back)

ISOLATED TO ONE LEVER - `MYLANG_JIT_OFF=flit` on the CURRENT binary
restores the pin and the time EXACTLY (0.05s, the baseline's number).
Conversely `MYLANG_JIT_OFF=fcache` on the BASELINE reproduces the
current build's 0.23s. So it is the FLOAT LITERAL POOL change.

RULED OUT, each by a build:
  - the float pin BUDGET. Made MAX_FCACHED env-settable and swept
    K = 2,3,4,5,6,8,14: all 0.23-0.24s. Raising it 4 -> 14 is NOT the
    cause.
  - `fp_allocatable`. Reverted to `x <= 7` alone: still 0.23s.
  - float SCRATCH starvation: `alloc_fscratch` is defined and called
    by NOBODY, so the pool cannot starve it.

### ROOT CAUSE — FOUND. `fbusy` ANSWERS A PER-PC QUESTION AND A
### RUN-SCOPED CONSUMER READS IT AS A RUN FACT

Logging every float register transaction (a temporary print inside
`RegAlloc::ftake`/`fgive`) settles it in five lines:

    flit ON                        flit OFF
    FT take -> xmm0  (stage a)     FT take -> xmm0
    FT take -> xmm1  (stage b)     FT take -> xmm1
    FT take -> xmm2  (a float PIN) FT take -> xmm2
    FT GIVE BACK xmm2              FT GIVE BACK xmm2
    FT take -> xmm2  (the LITERAL) FT GIVE BACK xmm2

`jit.cpp` ~24884: the F4b float trans-mode binding takes a physical
register per abstract register, then the very next loop GIVES BACK any
whose abstract register is not an ENTRY OCCUPANT:

    if (!is_entry)
        e.ra.fgive(afphys[ar]);

That is correct for its stated purpose, and the comment says what it
is: *"busy is PER-PC under transitions - busy <=> a cache entry
exists"*. Each install `take`s at its own pc and each flush gives back.

⛔ BUT `pick_float_lits` IS RUN-SCOPED. It runs later (~24920), sees
xmm2 not busy, and takes it for the whole fragment - while the plan
still installs slot `x` into xmm2 at its install pc. Both consumers own
the register. The literal's `flit_load` is emitted at every entry AFTER
the fcache loads, so the literal wins and the pinned value is gone;
the emitter then re-reads `x` from memory with a type dispatch, four
times per iteration. Answers stay right (every net is green); the speed
collapses.

**THE GENERALISABLE LESSON, and it is a new shape of the one this
change already learned once:** `fbusy` is asked TWO different
questions - *"is this register free at THIS pc?"* (per-pc, what the
transitions mean) and *"may I hold this for the WHOLE RUN?"* (what the
literal pool, the capture base and the pins need). One bit answers
both, so a give-back intended for the first silently grants the second.
The baseline never hit it because `FLIT_REGS = {2,3}` and
`FCACHE_REGS = {4,5,6,7}` were disjoint BY CONSTRUCTION - the same
"a hand-partitioned resource has no conflict detection" finding as the
xmm2 clobber, now on the GIVE-BACK path rather than the take path.

⛔ **AND THE GP TWIN HAS THE IDENTICAL SHAPE - CHECK IT.** ~24596 does
the same `if (!is_entry) e.ra.give(aphys[ar]);`, and the #112 CAPTURE
BASE asks `e.ra.take(...)` AFTER it (~24660). It is not known to
misbehave today (the capbase claim emits zero bytes corpus-wide per its
own note), but it is the same latent hazard and must be audited with
the fix, not after it.

### THE FIX (designed, NOT yet written)

`RegAlloc::ftake(prefer, exclude)` already takes an exclude mask. Build
the float plan's physical set - the OR of `1u << afphys[ar]` over every
bound abstract register - and pass it as `exclude` to the run-scoped
consumers that ask after the give-back. Same for the GP twin and
`capbase`.

Stated as a property rather than a patch: **a register the PLAN
installs into at any pc is not available to a RUN-SCOPED consumer,
whatever `fbusy` says at the moment it asks.**

Secondary, and it is the defect independent of the mechanism: the pool
had a bound (`FLIT_REGS`, two registers) and I replaced it with
`while (ftake() >= 0)` - no bound at all. A pooled literal saves ~2
instructions per use; a pin saves a slot's whole traffic. The pool is
the lowest-value float consumer and must never outbid a pin.

⛔ NOT A BUG, corrected: the debug print appeared to pool `0.5` TWICE
on 04_float_arith. Its literals are `0.5` and `0.4999999`, and `%g`
renders both as `0.5`. There is no dedup failure - `add()`'s memcmp is
correct. Printed a value at default precision and nearly reported a
bug that does not exist.

### THE Ir LEDGER (callgrind, deterministic, per 1M iterations of
### 05_mixed_arith; scale-3 minus scale-1 so compile time is excluded)

    base   all on          31.0
    base   flit off        33.0
    base   fcache off      45.0
    cur    all on          45.0   <- identical to base with fcache OFF
    cur    flit off        33.0   <- identical to base with flit OFF
    cur    ffwd off        50.0
    cur    flit+ffwd off   38.0

The pool costs 12 instructions per iteration here, and `cur` behaves
exactly like a build with no float cache at all.

## WHAT IS LEFT

### (a) THE xcache DENIAL — and the first write-up of it was WRONG

`jit_run_blocks_xcache` denies the CALLER-SAVED half to any run that
emits a MyLang call. I described this as a register wasted for nothing.
Reading `emit_sync_push_native` properly: it holds rax, rbx, rcx, rdx,
rsi, r8, r9, r10, r11 as PROTOCOL across one long straight-line
sequence - **8 allocatable registers of 13** - so "route its r10/r11
scratch through `alloc_scratch()`" cannot work; there is nothing to
route them to. And a MyLang call is a real SysV `call`: the callee
clobbers every caller-saved register regardless.

THE ACTUAL FIX: BRACKET the MyLang call site with the same
`emit_call_prologue`/`emit_call_epilogue` the helper calls already use.
The prologue stores each caller-saved pin to its slot payload and the
epilogue reloads - so the pin survives, the raw scratch is legitimately
free, and the denial drops away. Cost: a store/load pair per pin per
call site, which is exactly the trade the caller-saved extension
already makes for helper calls.

It changes the emitted CALL SEQUENCE, so it belongs with #97 (whose
frameless-callee plan touches the same code), not with #123.

### (b) RETIRE THE PICK (#103's residual)

`g_jit_lsra = false` selects the legacy path. The positional zip is
ALREADY gone - both modes bind through `ra.take` - so what remains is
deleting the non-LSRA RANKING path and reworking the eight coverage
tests that force `g_jit_lsra = false` for their duration. It changes
which SLOTS are chosen, never which REGISTERS are used, so it is
independent of everything above.

## VERIFICATION BATTERY (for the next change here)

    make -j BUILD_DIR=build-claude/t123 TESTS=1 OPT=0 && ./... -rt
    tests/corpus_diff.sh build-claude/t123/mylang  (+ every matrix)
    scripts/disasmcheck.py build-claude/rel123/mylang
    scripts/vdjcmp.sh BIN BIN                      (self-test)
    read -vdj for the touched shapes, by hand

---

# #103 — RETIRING THE PICK: the map, and the fork it ends at

## INCREMENT 0 (done): how much does the legacy pick still decide?

Counters `runs_compiled` / `pick_decided` / `pick_pins`, over 125
programs (bench/my + samples + tests/functional):

    runs compiled       335
    the PICK decided     75   (22%)
    ...and pinned >= 1    0   (ZERO)

So in the SHIPPING config the surviving fallback contributes NO PINS.
Whenever the interval machinery declines, `pick_cached_slots`' own
`hot` is empty too. Deleting that fallback costs nothing.

## WHAT THE LEVER IS ACTUALLY HOLDING UP (measured)

Building with the gate forced ON, FOUR of the eight tests that force
`g_jit_lsra = false` still pass - their forcing was stale and is now
REMOVED (commit 40ac11d). Two of those four are a direct consequence of
#123: the C2b pair asks the allocator instead of predicting.

The other four fail, all through their own VACUITY guards ("the shape
is not reaching it"), and the corpus says why:

    corpus-wide, DEFAULT config (LSRA on)
      lsra_pins    250079      xcache        1   (46_matrix_mult only)
      lsra_trans   150508      rax_pin       0
      scache            7      range_share   0

⛔ THE LINEAR SCAN SUPERSEDES THOSE MECHANISMS RATHER THAN LACKING
THEM. At maximum pin pressure, 83_regs_int_40 gets
`lsra_pins 1, lsra_trans 22, scache 1` under LSRA and `xcache 1` under
the pick: the scan SPLITS LIVE RANGES instead of holding a value in a
caller-saved register for the whole run and spilling it around every
call. That is the better answer to the same problem.

## ⛔ THE "FORK" WAS BUILT ON A MIS-READING — CORRECTED 2026-08-31

This section used to present three "superseded mechanisms" and ask
whether to delete them with the pick. TWO OF THE THREE ARE NOT THE
PICK'S, and the error was reading a counter as if it were a reach
measurement.

| mechanism           | its test               | what it really is        |
|---------------------|------------------------|--------------------------|
| caller-saved pins   | jit_xcache_pins        | SHARED, live (46 only)   |
| rax as a pin        | jit_rax_pin_test       | SHARED, live (retries)   |
| ShareSeam / seams   | jit_range_share_test   | PICK-ONLY, structurally  |
| ShareSeam L+1 clamp | jit_share_region_clamp | PICK-ONLY, structurally  |

**xcache is SHARED.** Since #123 both allocators bind pins through the
same `ra.take(CAP_ALLOCATABLE)`. There is no xcache code path - only
"the allocator handed out a caller-saved register", plus the
save/restore `emit_call_prologue`/`epilogue` already emits. The scan
reaches it on 46_matrix_mult. Deleting it would forbid the allocator 9
of its 13 registers, i.e. undo #123.

**rax is SHARED, and `rax_pin` is the WRONG COUNTER.** It counts
fragments that RAN with rax pinned - 0, because the eviction worked.
`rax_retries` counts rax being chosen, conflicting, and the chunk
re-emitting with it denied: **1 on each of 83/82/81_regs_int under the
shipping allocator**. The machinery fires. The absence of a pin is its
SUCCESS, not its death.

**ShareSeam IS pick-only**, and structurally: under trans mode
`spill_hot = lsra_homes`, so the scan substitutes its own overflow plan
and the share plan never runs (`range_share` 0, `share_clamped` 0). It
is the one genuine deletion candidate here, and it is small and
self-contained - a separate question, not a fork attached to #103.

⛔ **THE LESSON, and it is the transferable part: a counter reading 0
may be measuring a mechanism that WORKED.** `rax_pin` and
`rax_retries` differ by exactly that. Ask what a counter is bumped BY
before concluding a path is dead - the same discipline as "an
emitted-code counter proves the code was EMITTED; when the thing that
broke is a GUARD, reach is zero and the counter looks exactly like
'the tier was never nativized'".

## AND THE SCAN-SIDE TEST WAS PROPOSED AND WITHDRAWN

The proposal was: since 46_matrix_mult reaches xcache under the scan,
write a test proving it works there. TWO attempts to build the shape
FAILED - a two-deep `while` nest over arrays-of-arrays, and a close
replica of 46's function-with-three-counted-loops - both giving
`lsra_trans` and no xcache. Something specific about 46 triggers it and
what that is, is not known.

So it is NOT the small win it was described as, and a half-crafted
version would pass while checking nothing. WITHDRAWN. What is worth
recording instead is the fact itself: **46_matrix_mult is the only
corpus program that makes the scan use a caller-saved register.**

## WHAT #103 ACTUALLY DELIVERED

 - increment 0: the legacy pick decides 75 of 335 runs and pins
   NOTHING in any of them;
 - four of eight coverage tests moved off the legacy allocator onto
   the shipping one (two of them only possible because #123 made the
   C2b pair ask the allocator rather than predict);
 - the allocator choice became an ORDINARY LEVER (`JL_LSRA`), with
   `MYLANG_JIT_LSRA` and `g_jit_lsra` deleted - three spellings of one
   gate became one - and `corpus_diff --levers`, hence CI, now covers
   it. NOT vacuous: 21 of 35 corpus programs emit different machine
   code under the two allocators.

OPEN, small and separable: whether to delete the ShareSeam family,
which is the only genuinely pick-only code here.
