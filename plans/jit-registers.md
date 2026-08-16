# JIT registers: kill the pins, then allocate

Status (2026-08-15): **steps 1 and 2a LANDED; 2b's ranking half was
built and REVERTED as inert; 2b's cache-awareness arc is DONE for
locals; step 2c - the real allocator - is REFRAMED BY MEASUREMENT and
UNBUILT.** The short version of the reframe: the existing cache already
holds the hot LOCALS (177 pinned / 25 lost over the corpus), and the
remaining traffic - **~16 data references per iteration of a pure int
loop, against C++'s ~0.05** - is in the TEMPS, which the N5 rule
excludes. That exclusion is a property of the fragment-wide PIN model,
not of temps; live ranges dissolve it. See "Step 2c" below.
Direction set by the maintainer
2026-08-01: *"minimize the number of pinned registers, ideally to 0 but 1
is also acceptable. All the other ones should be allocated depending on
availability by the codegen. When we need more registers than what the CPU
offers, just save the values to the slots or to the stack and pop them
later, as GCC does."*

## Where we are

The JIT has no register allocation. Every register has a hard-coded role,
and **five registers are never used at all**:

    RDI   pinned fragment-wide: the frame slots base
    RSI   pinned: the t_int Type singleton
    R8    pinned: the t_float Type singleton
    R10   } the N5 cache - at most TWO hot int slots per fragment,
    R11   } chosen by pick_cached_slots (a heuristic, not allocation)
    RAX RCX RDX R9        per-op scratch, reloaded every time
    RBX R12 R13 R14 R15   ZERO uses in the entire emitter

Everything else round-trips through frame slots in memory.

Every pinned register is **caller-saved**, which is why
`emit_call_prologue` / `emit_call_epilogue` exist: each helper call pushes
RDI plus the cache registers, pads for alignment, calls, pops, and then
RELOADS the two type singletons with two `movabs`. That is ~4-8
instructions around every helper call, and helper calls are frequent -
76_funcval_dispatch makes four per iteration (jit_subscript, jit_move,
jit_put_int, jit_bind_ref_arg).

Worse, RDI does double duty: it is the slots base AND the first argument
register, so `lea rdi, [rdi+off]` to form a helper argument DESTROYS the
base. That is the whole reason for the push.

## Step 1 - move the pins to callee-saved registers

    slots base   RDI -> RBX      <-- DONE
    t_int        RSI -> R12      <-- not done; see "what step 1 left"
    t_float      R8  -> R13      <-- not done
    N5 cache     R10/R11 -> R14/R15   <-- SUBSUMED by step 2

Then `emit_call_prologue` / `emit_call_epilogue` become **no-ops**: the
callee preserves all four/five, and RSI/R8/R10/R11/RDI become free
scratch and argument registers.

### What landed, and what it actually measured

The base moved to RBX exactly as sketched below. `emit_call_prologue` no
longer touches the base at all; what remains in it is the N5 cache spill,
which step 2 deletes.

MEASURED (callgrind Ir, `OPT=1 ASSERTS=0` on both sides): 43_sieve
**-1.31%**, 76_funcval_dispatch -0.75%, 11_closure_counter -0.59%,
46_matrix_mult -0.52%, 63_closures -0.38%, 10_recursion_deep -0.31%;
01_while_loop and 09_fib_recursive EXACTLY neutral; 35_map_filter +0.30%
and 34_sort_custom_cmp +0.25%.

**The ~3% estimate below was WRONG and the reason is worth keeping.** The
saving is 2 instructions (`push rdi`/`pop rdi`) per helper CALL, but the
cost is 2 instructions (`push rbx`/`pop rbx`) per fragment ENTRY - and in
the shapes that matter those frequencies are similar. Deep recursion
enters a fragment per call; the callback benches (map/filter/sort) re-enter
one PER ELEMENT through `VmInvoker`, which is why those two went slightly
the wrong way. Only where a fragment makes several helper calls per entry
(a sieve's inner loop) does it pay clearly.

So step 1 is not a perf change. It is the enabling change: an allocator
over four caller-saved registers would have to spill everything around
every helper call, which is most of what an allocator is for.

## Step 2a - the cache pool moves to r12-r15 and widens (LANDED)

The N5 cache registers were r10/r11 (caller-saved, spilled around every
helper call, at most two slots). They are now r12-r15 (`CACHE_REGS` /
`MAX_CACHED`): callee-saved, so the spill moves from once per CALL to one
push at `frag_entry`, and the pool is twice as wide. `emit_call_prologue`
became an empty marker; the epilogue is just the two singleton `movabs`es.

**The exit had to be shared.** `exit_pc` inlined the whole tail, so its
size grew with the pool - at four pinned slots the flush alone is 56
bytes, and the short `jcc` several guards use to hop OVER an exit ran out
of its 8-bit displacement (a `patch8` assertion, caught by `-rt`). An exit
is now `mov eax, pc; jmp <epilogue>`, a constant 10 bytes, with the tail
emitted ONCE per fragment. TWO tails: a barrier'd op empties the cache on
purpose and its exit must NOT flush.

Measured (callgrind Ir, cumulative with the rbx move): 43_sieve **-5.07%**,
14_array_subscript **-3.20%**, 46_matrix_mult **-2.82%**,
76_funcval_dispatch -0.75%, 11_closure_counter -0.59%, 63_closures -0.39%,
10_recursion_deep -0.31%; 01_while_loop / 07_nested_loops exactly neutral;
35_map_filter +0.30%, 34_sort_custom_cmp +0.25%.

**The pool is not full, and that is the point of step 2b.** A four-wide
pool fed by a "used >= 3 times, top N by count" heuristic still pins ONE
register in a loop with four hot accumulators, because `a += i` counts as
two uses of `a` and the threshold rejects it. The registers are available
now; what is missing is an allocator that decides from LIVE RANGES.

### What step 1 deliberately left alone

`rsi` (t_int) and `r8` (t_float) are still pinned and still re-materialised
by `emit_call_epilogue`. They are NOT worth moving to r12/r13 on their own:
they are two `movabs` of a compile-time constant, i.e. rematerialisable at
zero cost, and holding them in callee-saved registers would take two
registers away from the allocator to save two instructions per call. The
right treatment is to let step 2's allocator own them as ordinary
rematerialisable constants and spend r12/r13 on live values instead.

### The change surface is small - it is NOT the ~213 raw mentions

Almost every slot access goes through five Emitter encoders that hardcode
rdi in the modrm byte (`0x87` = mod 10, rm 111 = rdi). Change those and
the call sites follow for free:

    Emitter::load   (455)   mov  reg, [rdi+disp]
    Emitter::store  (474)   mov  [rdi+disp], reg
    Emitter::lea    (551)   lea  reg, [rdi+disp]     (+ lea_rdi)
    Emitter::fload  (527)   movsd xmm, [rdi+disp]
    Emitter::fstore (533)   movsd [rdi+disp], xmm

RBX is rm 011, so the modrm byte becomes `0x83`. Ten byte constants in
total. Everything reached through `read_slot` / `write_slot` /
`load_operand` / `off()` then moves with them.

The remaining explicit RDI uses are helper-ARGUMENT setup and stay exactly
as they are - that is what RDI is for.

### Entry / exit protocol

A fragment is called from C++ (`jit_enter`), so it must now save and
restore what it uses.

  - ENTRY: push the callee-saved registers this fragment actually uses,
    then load the base + any singleton it needs. Push an EVEN count (or
    pad) - at fragment entry `rsp % 16 == 8`, and the existing sites rely
    on the post-prologue state being call-ready.
  - EXIT: **`Emitter::exit_pc` (522) is a SINGLE emit point covering all
    101 exit sites** - `flush_cache(); mov eax, pc; ret`. Add the pops
    there and every ordinary exit is covered.
  - There are **12 other raw `u8(0xC3)` returns** (jit_ret's sentinel
    paths, the boundary/switch returns, emit_raise). Each needs the same
    epilogue. Enumerate them; do not pattern-match.

Only push what the fragment uses: a fragment with no float ops needs no
R13, one that caches nothing needs no R14/R15. A tiny leaf callee entered
per call should pay one push/pop, not five - otherwise this LOSES on the
call benches, where the callee fragment is entered per call while the
caller's is entered once per loop.

### Expected payoff (PREDICTED ~3%; the real figure was ~0.5% - see above)

Removing ~4-8 instructions from every helper call. On 76 that is four
helper calls per iteration, so roughly 3% of that benchmark - modest by
itself. The real reason to do it first is that it is the PREREQUISITE for
step 2: with the pins in callee-saved registers, an allocator has a real
register file to work with instead of four scratch registers it must
spill around every call.

### Two traps found while doing it

1. **Helpers took the slot window as their first argument IMPLICITLY**,
   because the base already WAS rdi. Moving the base broke seven of them
   with a null-deref inside `LValue::put` - caught by `-rt` on the first
   run, but it would have been a silent wrong-address write if the slot
   arithmetic had happened to land somewhere mapped. Those sites now say
   `slots_to_arg0()`. Putting the move in the prologue instead was
   measured to be dead code at 64 of 73 call sites.
2. **The 16-alignment parity INVERTED.** Entry is `rsp % 16 == 8`, so the
   body used to need an ODD push count at a call and now needs an EVEN
   one. `emit_call_prologue`'s pad rule survives unchanged only because
   exactly one push left it and exactly one entered at `frag_entry`; the
   two hand-spilling sites in the inline call push each had to swap a
   live `push rdi` for a `sub rsp,8` pad. A miss here is not a crash on
   x86-64 until some callee uses an aligned SSE store - i.e. exactly the
   kind of bug that hides.

## Step 2b - the allocator: ATTEMPTED, and the finding that stopped it

I built the selection half of a linear-scan allocator - live ranges per
slot plus a spill cost weighted by LOOP-NESTING DEPTH (back edges give the
depth, a use is worth 8x per level), replacing the flat "used >= 3 times,
top N by raw count". It is the textbook heuristic and it ranked correctly.

**It was inert, and I reverted it.** Isolated on its own (both binaries
`OPT=1 ASSERTS=0`, callgrind Ir): 44_primes_sqrt **+0.208%**, 46_matrix_mult
+0.035%, 68_nested -0.018%. Net slightly NEGATIVE, and - the point - the
register pool still did not fill.

**THE BINDING CONSTRAINT IS NOT THE RANKING. It is per-op cache-awareness.**
`pick_cached_slots` DISQUALIFIES a slot outright, for the whole fragment,
if any op in the run reads or writes it from MEMORY rather than through
the cache (`bad(...)` - 59 call sites). So the candidate set is small
before ranking ever runs, and a better ordering has almost nothing to
choose between.

The demonstration is a four-accumulator loop:

    var a = 0; var b = 0; var c = 0; var d = 0;
    for (var i = 0; i < 100; i++) { a += i; b += a; c += b; d += c; }
    print(a, b, c, d);

Four registers are free and every accumulator is used twice per iteration,
yet ONLY `i` gets pinned. Not because of the threshold - because the
`print` lowers to `move r5 = a` ... and `MoveV`'s emit copies a whole
32-byte EvalValue from memory, so it calls `bad()` on `a`, `b`, `c` and
`d`. One trailing op costs them the register for the entire fragment.

**So the next increment is to make the disqualifying ops CACHE-AWARE**, one
at a time, exactly as the model flip nativized ops one at a time to shrink
its islands. Only once the candidate sets are big enough for the choice to
matter does a smarter ranking pay, and THEN the live-range work above can
come back off the shelf.

### DONE: MoveV (the first one)

The source side reads the register - `store_dst(sreg, dst)`, the same
two-store as any int result, no reference check on either side - so
`bad(in.target2)` is gone. The four-accumulator loop above now pins all
four accumulators instead of only the counter.

**The soundness anchor is that a MoveV contributes NO WEIGHT.** It stopped
calling `bad`; it does NOT start calling `usei`. A MoveV is the BOXED move,
so the bytecode says nothing about the value's type and it must never be
the evidence that a slot holds an int. Zero weight means a slot reaches the
cache only when a genuine int op qualified it - and once it has, every
write to it in the run is an int write, so the value read here really is an
int. The DEST stays memory-only for the mirror reason: a MoveV can write
ANY type, so a pinned dst could silently stop holding one.

Measured (callgrind Ir, `OPT=1 ASSERTS=0`, this change alone):
01_while_loop **-5.92%**, 07_nested_loops **-5.43%**, 03_int_arith
**-4.00%**; everything else within +-0.09%.

### The audit exists now: MYLANG_CACHEAUDIT=1

`pick_cached_slots` counts, per opcode, the slot candidacies its `bad()`
killed - a slot that is a resolved local and used often enough to have
cleared the threshold, so only the disqualification stopped it. Printed at
exit. Over bench/ + samples/ (85 programs), AFTER the MoveV change:

    pinned 177   lost 27

    MoveV             6      (the DEST side, which must stay memory-only)
    StoreElemValue    6
    FloatMulRI        5      | float slots - correctly disqualified,
    CompoundV         4      | an int cache can never hold them
    DictStore         4
    SubscriptV        2
    FloatSubRR        1
    BinOpV            1

Two things that reads off immediately: the remaining opportunity is SMALL
(27 lost against 177 pinned, and ~6 of the 27 are float slots an int cache
could never take), and no single site is worth much.

### THE RULE for which bad() sites are worth removing

There are two kinds, and only one is addressable:

  - the op can read the REGISTER (MoveV: the copy is just an int store).
    Removing its bad() is free and a clear win.
  - the op's helper must see MEMORY - anything taking `&slot`, or reading
    the frame through `g_current_ctx`: the container stores, the boxed
    ladder, the subscript read. Keeping the value in a register then means
    writing it back before EVERY execution of that op.

The second case was BUILT AND MEASURED - flush the one operand at the emit
(two stores, no reload, much cheaper than marking the op a barrier, which
flushes and reloads the whole pool) and stop disqualifying it. It does not
pay: 23_dict_insert -0.42%, but 46_matrix_mult +0.27%, 43_sieve +0.04%,
14_array_subscript +0.03%, 56_sieve_bool +0.01%, 62_dict_word_count
+0.00%. The flush costs 2 stores per ITERATION while the register saves
~1 load per other use of the counter, so it lands on zero. **The
disqualification at those sites is the RIGHT trade, not an oversight** -
reverted, and the reason is recorded above `pick_cached_slots`.

So the cache-awareness arc is DONE unless a new op appears that can read
its operand from a register. Check the audit first, then check which kind
the site is.

Recorded rather than kept, because shipping a measurable-but-negative
change that provably does not do its job is worse than a written finding.

## Step 2c - THE ALLOCATOR: REFRAMED BY MEASUREMENT (2026-08-15)

The old sketch here said "linear-scan over live ranges, and keep the N5
rule: only LOCALS, never TEMPS". **Both halves are now known to be wrong
about where the work is.** What follows replaces it.

### The measurement that reframes it

Data references (cachegrind `--cache-sim=yes`), release lane, scale 1:

| bench | MyLang D refs | C++ D refs | ratio |
|---|---|---|---|
| 03_int_arith | **16,340,036** (6.5M rd + **9.8M wr**) | 48,553 | **336x** |
| 08_func_call | 12,288,236 (4.2M rd + 8.1M wr) | 48,553 | 253x |

03_int_arith runs ~1M iterations, so that is **~16 data references per
iteration of a pure int loop** against C++'s ~0.05. This is the single
largest measured gap in the project, it is squarely in the category that
reliably WINS here (memory traffic - see plans/cpp-gap-ladder.md), and
**more of it is WRITES than reads**, which is the tell: every op stores
its result back to a frame slot, and an int store is TWO writes (payload
+ type tag).

### It is NOT the locals. It is the TEMPS.

Reading 03_int_arith's emitted loop (`-vdj`, the disassembler renders
slots by name) shows the three locals `acc`, `i`, `N` already living in
r12/r13/r14 - the N5 cache is doing its job. What round-trips through
memory is `r4`, `r5`, ... - the TEMPS:

        mov  rcx, r4.type          ; a type-tag load on a temp
        mov  rcx, [rcx+0x8]
        cmp  rcx, 8
        lea  rdi, r4               ; &slot handed to a helper

The corpus audit agrees from the other side: **177 pinned / 25 lost**,
and ~6 of the 25 are float slots an int cache could never take. The
LOCAL side is at its ceiling. Nothing in the audit can see temps,
because a temp is never a candidate.

### Why temps were excluded, and why that reason does not survive

The N5 rule says a temp may never be cached, because "a temp is scratch
the VM reuses across run boundaries (an int inside one run, a foreach
snapshot or a slice temp between runs), so an eager entry-load /
exit-flush would overwrite a live container with an int and its type
tag" - a FUZZER find, recorded in plans/archived/native-aot.md N5.

**That is a property of the PIN model, not of temps.** An N5 pin is
fragment-wide: load at entry, flush at every exit. It therefore touches
the slot at moments when the temp legitimately holds something else.

A LIVE-RANGE allocation does not. A temp's range is almost always
`[def, last use]` INSIDE one run - one op produces it, the next consumes
it. Allocate only over that range, with **no entry load and no exit
flush outside it**, and the slot is never read or written at a moment
when its type is unknown. The soundness objection disappears, and it
disappears exactly where the traffic is.

This is the same shape as #94's borrow: the old rule was sound but
stated over the wrong unit (there, "the whole chunk's ref_slots"; here,
"the whole fragment").

### The three tiers, in increasing order of proof obligation

**T1 - INT/FLOAT TEMPS with a within-run live range.** The bulk of the
16 refs/iteration. A temp defined and consumed inside one run needs no
slot at all: it is a virtual register. Predicted saving: the payload
store, the type-tag store (C3 already elides that for qualified LOCALS -
`Emitter::tflush`; temps got only the FLOAT half, C3 inc 3), and the
reload. **Estimate the ceiling from the `-vdj` count before building.**

**T2 - SPILL/RELOAD instead of disqualification.** Today `bad()` removes
a slot from the pool for the WHOLE fragment if any op needs it in memory.
With live ranges the answer is to split the range and spill around that
one op. NOTE step 2b already measured the naive form of this - flush the
one operand at the emit - and it landed on ZERO (23_dict_insert -0.42%,
46_matrix_mult +0.27%, others ~0). So T2 is only worth building if T1's
range splitting makes it free, and it must be measured separately.

**T3 - REFERENCES IN REGISTERS (the maintainer's second ask).** A
reference-typed value is a `shobj` pointer plus `off`/`len`/`slice`; the
reason it cannot simply live in a register is the REFCOUNT - a register
copy does not retain, and the frame slot is what the release scan walks.

**#94 is exactly the missing proof.** A BORROWED reference is one the
escape analysis proved cannot outlive the call, bound with no retain
and abandoned rather than released at the pop - i.e. a reference whose
refcount is somebody else's problem for the whole of its lifetime. That
is precisely the condition under which its pointer is just a value and
may live in a register. Two sub-cases, in order:
  - a borrowed ARGUMENT slot the callee only READS (the #94 bit already
    proves non-escape; `frame_release` already knows not to release it);
  - a loop-invariant container LOCAL (`a` in `a[i]`), where the owner is
    the caller's slot for the whole loop. Check first whether the C1
    navigation hoist (`jit_hoist_c1`) already covers the shape - it
    hoists per-loop navigation and may leave nothing to win.

T3 must NOT precede T1: it has the hardest soundness argument and the
smallest measured traffic.

### ⛔ THE CEILING IS MEASURED, AND IT IS REGIME-DEPENDENT (2026-08-15)

The census asked for below was done, and it does NOT support building
this on the shape it was planned around. `bench/micro/slotcost.cpp` runs
03_int_arith's arithmetic in registers versus round-tripped through a
frame slot at the real 48-byte stride plus the tag store:

  - latency-bound loop (a serial chain through `acc`): **1.00x - FREE**
  - throughput-bound loop (4 independent accumulators): **3.21x**

and 03_int_arith - the shape with ELEVEN slot references per iteration -
is the FIRST kind. Corroborated three ways: its D1 misses (~50,000) are
identical to two other benches with wildly different reference counts,
so the loops miss essentially never; it runs 63 instructions per
iteration in 3.89 ns against a register-only C++ chain's 3.30 ns, so all
the extra work costs 0.59 ns; and removing two of its eight stores
measured -35.84% data references for 0.998x wall clock.

**So the payoff is not "MyLang does 336x the data references". It is
(fraction of hot loops that are THROUGHPUT-BOUND) x (up to 3.2x).** The
discriminator is already in the bench table: a benchmark near the ~2.3x
my/cpp floor is latency-bound and this buys nothing there; one far above
it (76_funcval_dispatch at ~11x) is where the traffic is not hidden.

**Rank the corpus by my/cpp and census the WORST benches, not the ones
with the most slot references.** Those are different sets, and conflating
them is why this plan pointed at 03_int_arith.

### AND THE CENSUS OF THE WORST BENCHES SAYS: NOT THIS (2026-08-15)

Ranking the corpus by STARTUP-CORRECTED my/cpp (scale-3 minus scale-1 on
both sides) and classifying each worst loop's emitted body:

| bench | loop my/cpp | instrs | calls | slot ld/st | dominant |
|---|---|---|---|---|---|
| 30_str_index_iterate | 27.36x | 30 | 1 | 2 / 0 | CALL |
| 63_closures | 17.57x | 151 | 6 | 15 / 7 | CALL |
| 76_funcval_dispatch | 11.40x | 248 | 6 | 11 / 4 | CALL |
| 11_closure_counter | 11.11x | 259 | 15 | 18 / 6 | CALL |
| 64_struct_create | 10.26x | 137 | **0** | 22 / 14 | **slots** |
| 75_indexed_unpack | 9.18x | 92 | 5 | 4 / 4 | CALL |
| 73_multi_unpack | 8.47x | 55 | 1 | 2 / 0 | CALL |

**Seven of the eight are CALL-dominated.** Slot traffic dominates exactly
one, and 03_int_arith - the shape this plan was written around - measures
**1.18x of C++** once startup is removed, i.e. it is already at parity
and has nothing to give.

**So build this for 64_struct_create or not at all** - and the gate said
MEASURE that shape first. It was measured (2026-08-16), and the answer is
**NOT AN ALLOCATOR EITHER**.

### What 64_struct_create's loop is actually made of

Reading the emitted 137 instructions: **~60 of them are TEN blocks of
runtime FLOAT TYPE-DISPATCH**, all of this shape -

        mov   rax, <slot>.type
        cmp   rax, r8              ; the t_float singleton
        jne   cold
        movsd xmm0, <slot>
        jmp   done
    cold: cvtsi2sd xmm0, <slot>

- and they dispatch on values whose type is ALREADY KNOWN. Three read
`i`, a statically-INT local, so the conversion is unconditional by
construction. Seven read a TEMP the same run tagged a few instructions
earlier (`mov r7.type, r8; movsd r7, xmm0` ... then a dispatch on
`r7.type`).

**And the cache has nothing to allocate here anyway**:
`MYLANG_CACHEAUDIT=1` on this bench reports **pinned 2, lost 1**
(FloatMulRI). The dispatched values are TEMPS, which the N5 rule keeps
out of the int cache, and `i`, which is already pinned. None of the
float levers engage at all - no `fread`, no `telide`, no `fcache`
counter fires on this program.

### The ceiling, measured

`bench/micro/slotcost.cpp` gained a model of this shape - the same
arithmetic with the intermediates in registers versus round-tripped
through 48-byte slots WITH the tag store and the dispatch:

    RS  registers     0.45 ns/iter   1.00x
    MS  +slots+disp   0.94 ns/iter   **2.07x**

So the pattern is NOT free - unlike the dead stores of 03_int_arith,
this one is in the paying regime. But 0.94 ns is against the real loop's
**9.85 ns/iteration**, and the model carries 3 of the loop's ~10
dispatch blocks, so eliminating the pattern entirely is worth roughly
**5-16%** of this benchmark - the low end of the 0-26% bound, and only
on this benchmark.

### The conclusion

**The 60 instructions are not a register-allocation problem.** They are
the emitter dispatching on types IT JUST SET. The targeted fix is to
extend the existing float type-proof (C4a `fread` / C3 `telide`) to two
cases it currently misses:
  a. an int-typed LOCAL read as a float operand - `cvtsi2sd`
     unconditionally, no dispatch;
  b. a TEMP the same run already tagged - `movsd` unconditionally.

That is smaller than an allocator, targets the actual instructions, and
reuses levers that exist. **Predict before building** (the ladder's
rule): those blocks are a type LOAD plus a perfectly-predicted branch,
which the guard-elision entry says retires nearly free - so the honest
expectation is well under the 2.07x the isolated model shows, and this
should be measured on the wall clock before anyone commits to it.

### ⛔ THE REGISTER BUDGET, CORRECTLY STATED (maintainer, 2026-08-16)

I first told the maintainer "MAX_CACHED = 4 is the hard limit". **That is
wrong as stated**, and the correction matters because it changes what is
buildable.

The 16 GP registers today:

| register | role |
|---|---|
| RSP, RBP | stack / frame pointer. RBP is LOAD-BEARING: the
  no-record tier walks the live rbp chain to rebuild frames |
| RBX | the frame slots base - `MODRM_SLOT = 0x83` bakes
  rm=011 into every slot access |
| RSI, R8 | the t_int / t_float singletons, pinned and
  re-materialised after every helper call |
| RAX RCX RDX RDI R9 R10 R11 | **used constantly, as per-op SCRATCH** |
| R12-R15 | `CACHE_REGS` |

**Four is the ceiling for PINNING, not for ALLOCATION.** A pin holds a
register for the whole fragment, so it must survive a helper call, so it
must be callee-saved - and SysV's callee-saved set is exactly
{RBX, RBP, R12-R15}, of which two are taken. That is where 4 comes from.

A REAL allocator does not pin. It holds a value in a caller-saved
register and spills **only around calls that actually occur** - which is
what native code does, and what the maintainer correctly objected was
missing. **The loops that matter here make NO calls in the body**
(`bench/my/8*_regs_*`, 03_int_arith), so a caller-saved-capable
allocator would have **~11 GP registers free there with zero spilling**,
against the four it uses today.

### And why that could pay even though C++'s own spilling is free

The pressure family measured C++ spilling **35% of its loop instructions
at N=40 with no change in per-update cost** - so registers per se are
not worth much at that pressure. The reconciliation is the SHAPE of the
memory operation, not its presence:

  - a C++ spill is **ONE** 8-byte access of a raw `long`;
  - a MyLang slot round-trip is **THREE** - a payload store, a TYPE-TAG
    store, and a payload reload - 48 bytes apart.

So MyLang has strictly more to gain from a register than C++ does, and
C++'s indifference to spilling does NOT transfer. It also says the two
levers are alternatives aimed at the same cost: hold the value in a
register (an allocator), or make the slot cheaper (C3's tag elision,
and #96's redundant float dispatch). Either removes the same traffic.

**The experiment is already built.** `bench/my/8{0..5}_regs_*` with the
N=8 row as the cleanest number - C++ uses no stack at all there, and
MyLang still puts half its accumulators in memory.

### THE MANDATE (maintainer, 2026-08-16) - and it is NOT REVERTIBLE

*"I want the real register allocator done at 100%. I'm not going to
compromise on that. The benefits are unclear, but I wanna bet on that
and don't revert it please! If a change introduces a regression, that
change is incorrect, there's something you're missing. Debug, re-work
it, don't revert this effort."*

Target: the pinned set becomes **RBP, RSP and ONE more** (RBX, the slots
base) - **13 of 16 usable**. A register must be REUSABLE by several
variables in the same frame and the same block, evicted to reserved
native-stack space the way a C compiler does.

**A REGRESSION IS A BUG IN THE CHANGE, NOT A REASON TO BACK OUT.** That
inverts this file's earlier habit (step 2b was reverted as inert); under
this mandate the same finding means "debug it".

#### LANDED: the spill area

`Emitter::spill_slots` / `spill_bytes()` / `spill_off(k)` / `spill()` /
`reload()`. frag_entry carves the slots out of the native stack after
the pushes and the pad; frag_ret releases them first.

  - **RBP-relative, not RSP-relative** - a helper call site PUSHES
    registers around the call, so rsp moves inside the body and an
    `[rsp+k]` spill address would shift under it. rbp is fixed for the
    fragment's life.
  - **the byte count is rounded to 16**, so `entry_pad()`'s parity is
    unchanged - the alignment trap this file documents (a miss is not a
    crash until some callee uses an aligned SSE store).
  - inert at `spill_slots = 0`: emitted code is byte-identical, -rt
    1917/1917.

#### LANDED: lever A learns the SHIFT family (and what that says)

The FIRST thing the pressure benches showed, before any allocator work:
`bench/my/80_regs_int_08`'s loop was spending **three memory operations
per accumulator per iteration** on a temp alive for ONE instruction -
`t = a >> 3; a = a ^ t` - because `jit_fwd_producer`/`jit_fwd_consumer`
did not know IntShl/IntShr. The whitelists predate
`specialize_arith_ops` growing those opcodes and nothing re-audited
them; an unlisted op silently does not forward. **-21.0% Ir on
83_regs_int_40, -17.97% on 80_regs_int_08**, 11 corpus programs
changed, everything else byte-identical. Full record in
docs/jit-optimizations.md.

**This is NOT the allocator** and must not be mistaken for it - it is a
stale-table fix the allocator's own measurement uncovered. But it moves
the baseline the allocator will be measured against, and it makes the
N=8 row's remaining gap purely about the ACCUMULATORS (six of eight
still in memory), which is the thing only more registers can fix.

The lesson to carry into the allocator: **the cheapest register is the
one a value never leaves.** Before widening the pool, check whether the
traffic is a value that could simply have stayed where it was produced.

#### The order for the rest

1. **Per-instruction register STATE** replacing `pick_cached_slots`'
   whole-fragment pin - this is the allocator, and everything else is
   detail.
2. **Live ranges from `visit_use_def`** - reuse it; a second enumeration
   would rot (the audit-table trap).
3. **Spill the furthest next use** on pressure; reload on demand.
4. **Flush live registers at every exit.** `exit_pc`/`flush_cache`
   already does this for the pinned pair and generalises - but the
   **12 raw `u8(0xC3)` returns are NOT covered by it** and must be
   enumerated, not pattern-matched.
5. **Unpin RSI/R8** (the type singletons) LAST of the foundations: it is
   a REGRESSION on its own (+1 instruction per type store) until the
   allocator can choose to keep a hot singleton in a register. Land it
   WITH that ability, not before - this is exactly the "bundled change
   needs a kill switch" shape, so gate it.

**KEEP the N5 soundness rule's REASON** - a temp's slot is reused across
runs holding different types - but note it is a property of PINNING: a
live range confined to `[def, last use]` inside one run never touches
the slot at a moment when its type is unknown.

**MEASURE ON `bench/my/8{0..5}_regs_*`**, the N=8 row first: C++ uses no
stack at all there, and MyLang still puts half its accumulators in
memory.

### What to build first, and the gate before building it

1. **MEASURE THE CEILING.** For 03_int_arith and 08_func_call, count from
   `-vdj` how many per-iteration frame accesses are to temps with a
   within-run live range. Multiply by the iteration count and compare
   against the 16 refs/iteration total. That number is T1's ceiling, and
   per plans/cpp-gap-ladder.md it must be stated BEFORE any code is
   written. If it is not most of the 16, stop and re-plan.
2. **T1 for a single shape**: a temp defined and consumed by adjacent
   int ops inside one run. Reuse `visit_use_def` for the ranges - it is
   the audited use/def enumeration and a second one would rot (the
   audit-table trap).
3. Only then widen, and only on wall-clock evidence.

### Interaction with what already exists - do not rebuild these

  - **`visit_use_def`** (codegen.cpp) is the audited use/def
    enumeration. USE IT. A new opcode must already join it.
  - **lever A (`fwd`)** already forwards a value between ADJACENT ops in
    a register, skipping the slot entirely. T1 is the general form of
    lever A; measure how much lever A already captures before assuming
    T1's ceiling is the full 16.
  - **C3 (`telide`)** already elides the per-write type store for
    qualified locals and for float temps. T1 subsumes it for the temps
    it takes; keep them consistent or the tflush bookkeeping diverges.
  - **`exit_pc` / `flush_cache`** is the single exit point for all ~101
    exits, and the mechanism generalizes; **12 raw `u8(0xC3)` returns**
    are NOT covered by it and must be enumerated, not pattern-matched.

## Testing protocol (both steps)

A wrong modrm byte is a SILENT MISCOMPILE, not a build error, so:

  1. `-rt` on gcc-debug/ASan, clang-debug, and hardened release.
  2. `tests/nested_fuzz.py` - it found the N5 temp-corruption bug that
     hand-written tests missed, and it is the right net for exactly this
     class.
  3. The corpus differential with **`-nj` as the oracle**: JIT-on vs
     JIT-off output over bench/ + samples/. For an emitter change that is
     the meaningful comparison - the tree-walker differential does not
     exercise emitted code.
  4. `-vdj` on a representative fragment and READ the machine code. This
     is what the disassembler is for.
  5. callgrind Ir before/after at `OPT=1 ASSERTS=0`, per bench.

## Order

Step 1 then step 2; step 2 is not sensible without step 1. Do the base
register (RDI -> RBX) as its OWN increment and land it green before
touching the singletons or the cache - it is the largest single win and
the easiest to verify in isolation.
