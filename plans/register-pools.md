# THE REGISTER POOLS — why hard-coded lists still exist, and what is
# actually wrong (opened 2026-08-29, maintainer-raised)

Companion to `plans/frameless-callee.md`. The maintainer's question was:
*"we built a full generic register allocator — why is there still a
hard-coded `CACHE_REGS`?"* This file is the investigated answer, so the
call-protocol conversation can resume from it.

---

## 1. THE MODEL, stated plainly

There are THREE layers and they are usually conflated:

    (a) THE ALLOCATOR      jit_lsra_assign(..., int K, LsraOut &out, ...)
                           Takes a COUNT. Allocates ABSTRACT registers
                           0..K-1. Never sees a register number. Live
                           intervals, splitting, eviction by use density,
                           a slot in different registers over its life -
                           all of it register-agnostic. This layer is
                           generic and is what #96 built.

    (b) THE BUDGET         max_pins = MAX_CACHED + n_xcache      (jit.cpp
                           :23746). This is the K handed to (a).

    (c) THE BINDING POOL   e.take_reg(CACHE_REGS, MAX_CACHED), then
                           e.take_reg(xcache_regs(), MAX_XCACHED).
                           Maps abstract -> physical, first-free through
                           the register STATE.

`CACHE_REGS` lives in (c). It is not the allocator's algorithm.

## 2. HOW MANY REGISTERS ARE ACTUALLY AVAILABLE

    INTEGER   jit_pin_budget() = MAX_CACHED + MAX_XCACHED = 4 + 9 = 13
              `mylang -v` prints: jit_pins 13 (xcache 9 caller-saved)

              16 GPRs, 3 structurally reserved (rsp; rbp = frame anchor;
              rbx = slots base) -> 13 allocatable, and ALL 13 are in the
              pool. The int side already uses every register it has.

    FLOAT     FCACHE_REGS[] = { 4, 5, 6, 7 }   -> xmm4-xmm7, and that
              is the WHOLE pool. MAX_FCACHED caps it. There is NO float
              equivalent of the caller-saved extension: 4 of 16.

⛔ **THE FLOAT CAP IS THE REAL GAP.** There is no xmm capability table at
all (`grep -c xmm_caps` is 0) - no derivation, no validation, no stated
reason. And xmm0-7 are the SysV ARGUMENT registers while xmm8-15 are
not, so by the GP weight model's own +4 penalty for an argument register
the float pool is the four WORST choices on the machine.

## 3. THE HARD-CODED INT LIST IS PROVABLY REDUNDANT

`gp_caps()` (jit.cpp:1201) already encodes the derivation:

    | ((r == 3 || r == 5 || r >= 12) ? CAP_CALLEE_SAVED : 0u)
    | ((GP_RESERVED_MASK & (1u << r)) ? 0u : CAP_ALLOCATABLE)

Evaluated against the code's own definitions:

    CALLEE_SAVED : rbx rbp r12 r13 r14 r15
    ALLOCATABLE  : all but rsp, rbp, rbx
    INTERSECTION : r12 r13 r14 r15        == CACHE_REGS, exactly

And jit.cpp:11602 VALIDATES the array against that derivation. The check
proves the copy is CONSISTENT; it cannot prove it is COMPLETE. Free a
register tomorrow and the check stays green while the register is
silently never used - *"a test derived from a table can never find a
hole in that table"*.

The replacement was already built. jit.cpp:1213, the weight model, in
its own words: *"reproduces today's hand-written preference (r12-r15
first) as an OUTPUT of the model instead of an assumption baked into an
array's order."* And one layer down `GP_RESERVED_MASK` was ALREADY
converted from literals to a derivation from the role constants, for
exactly this reason (watched 2026-08-19: dropping rbx from a literal
mask left the whole suite green while the slots base became
allocatable). The job was done for the mask and stopped at the pools.

**Why they survived:** #103's open residual, "retire the pick". The
legacy pick binds positionally (`hot[h] -> CACHE_REGS[h]`), and
BYTE-IDENTICAL EMISSION was the acceptance criterion for flipping LSRA
on - the array is what made "the LSRA emits what the pick emitted"
checkable with `vdjcmp`. A reasonable scaffold for the flip; not a
reason to still be here.

**The cost is already paid once:** r9 was added to `XCACHE_ORDER` by
hand on the argument *"r9 is used in exactly TWO local scopes and both
are already safe"* - false, ~80 raw-scratch sites - and it was a
SHIPPING WRONG ANSWER for a day. A derived pool could not have had that
bug: r9's scratch uses would have had to be expressed as a reservation
or a capability to be excluded, instead of as a claim in a comment.

## 4. ⛔ BUT WIDENING THE INT POOL IS MEASURED WORTHLESS

`MYLANG_JIT_MAXPINS=N` exists to answer exactly this, and its recorded
sweep (jit.cpp:23760, OPT=1 ASSERTS=0) says:

    07_nested_loops   pin 1 -5.36%, pins 2..7 +0.00%
    01_while_loop     pin 2 -5.85%, pins 3..7 +0.00%
    80_regs_int_08    EVERY pin +0.00%
    83_regs_int_40    EVERY pin +0.00%   (40 hot int locals!)

    "On 83 the fragment is 1367 emitted instructions at N=0 and 1416 at
     N=7 - pinning makes the code BIGGER and the dynamic count
     identical."

So "use ALL the registers" is, for INTEGERS, already true (13 of 13) AND
already measured to buy nothing past the second pin. Any int-side pool
work is a CORRECTNESS/maintainability change, not a performance one, and
must be sold as such.

## 5. ⛔ THE FINDING THAT MATTERS FOR #97

`n_xcache` counts the caller-saved members this RUN may spend, and
`jit_xcache_clobber` denies **the whole pool in a run that emits a
MyLang call** (jit.cpp:23659).

    a fragment containing a MyLang call  ->  max_pins = MAX_CACHED = 4

So the call-heavy fragments the frameless work targets - fib$0, the
closure bodies - are running on a FOUR-register budget, not 13. That is
not a hard-coded-list problem; it is a consequence of the caller-saved
registers being unusable across a call that the emitter itself makes.
Whether it costs anything is unmeasured: `MYLANG_JIT_MAXPINS` sweeps
DOWN from the budget, and the question here is what the fragment would
do with MORE. Worth a measurement before any of it is treated as a
problem.

## 6. WHAT TO DO — proposed, not started

 1. **Derive the int pools** from `{CAP_ALLOCATABLE ^ CAP_CALLEE_SAVED}`
    and order them by the existing weight model; delete `CACHE_REGS` and
    `XCACHE_ORDER`. Acceptance: emission BYTE-IDENTICAL corpus-wide via
    `scripts/vdjcmp.sh`. If it is not identical, the diff names the
    register the hand-written list was getting wrong. Sold as
    maintainability + closing the r9 class, NOT as speed (§4).
 2. **Build the xmm capability table** the GP side has, and derive the
    float pool from it the same way.
 3. **Then measure whether the float pool should be wider than 4**, and
    whether it should prefer xmm8-15 over the SysV argument registers.
    §4 is the warning: do not assume it pays. Sweep it.
 4. **Measure what a call-containing fragment would do with more than 4
    pins** (§5) before treating the xcache denial as a problem.
