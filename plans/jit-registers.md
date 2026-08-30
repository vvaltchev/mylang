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

#### LANDED: the caller-saved extension r10/r11 - the pool is 6 wide

`XCACHE_REGS`, `MYLANG_JIT_OFF=xcache`, JITSTATS `xcache`. The first
registers this JIT uses that SysV does not preserve for it: spilled by
`emit_call_prologue` and reloaded by `emit_call_epilogue` (the spill
lands BEFORE the arg setup at every call site, which is the correctness
argument), and taken only when no C1 hoist region wants the pair.

**MEASURED: -23.3% data references on 80_regs_int_08, and the wall clock
does not move** (1.004 / 0.985 / 0.984 / 1.052 across N = 8/14/25/40).
Full record + the two defensive-not-proven gates in
docs/jit-optimizations.md.

⛔ **THE FINDING THAT REDIRECTS THE REST OF THIS PLAN.** Ir is flat BY
CONSTRUCTION: `mov rax, [rbx+0x38]` and `mov rax, r10` are both one
instruction and one uop. An allocator removes DATA references, and on
this core an L1-hitting slot round-trip retires alongside the real work
- so **a data-reference win with an unchanged instruction count has a
wall-clock ceiling near zero**, the exact mirror of the guard-elision
family's instruction-count win with the same ceiling.

The two-regime model above predicted a payoff here and was WRONG about
this shape: `slotcost.cpp` varied the memory ops while the instruction
count ALSO changed, which is not what an allocator does.

**So a register is a PREREQUISITE, not the win.** The win is the
instruction the register makes removable:

    mov rax, r14      ; a0
    mov rcx, r13      ; i
    add rax, rcx
    mov r14, rax      ; a0 = ...
    mov rax, r14      ; <-- REDUNDANT: rax already holds it
    sar rax, 3

One instruction per accumulator per iteration - ~8 of 80_regs_int_08's
~50 loop instructions - and only removable once the value is in a
register at all. **That is the next increment**, and it is lever A
generalised from TEMPS to LOCALS (the plan's own T1 line, arrived at
from the other direction).

**DONE.** It needed no new carrier at all: lever A's `g_fwd` one-shot
already has exactly the right lifetime, and the temp restriction that
blocked locals belonged to the WRITE ELISION rather than to the read. So
the change is one condition MOVED - out of the arming test, down into
`skip_write`, which is also where `tb` stops being negative (`1 << tb`
for a local is UB).

MEASURED: 80_regs_int_08's loop body **92 -> 84 instructions**, the 8
predicted; **83_regs_int_40 -8.86% Ir and 0.89x WALL CLOCK**; 28 corpus
programs changed and every one Ir-negative or flat. Record in
docs/jit-optimizations.md.

**It confirms the corrected cost model.** Same benchmark family, same
box: removing 23.3% of the DATA REFERENCES bought 1.004x, removing 7.3%
of the INSTRUCTIONS bought a real win. The discriminator is the
instruction count.

#### The order for the rest

1. **Per-instruction register STATE** replacing `pick_cached_slots`'
   whole-fragment pin - this is the allocator, and everything else is
   detail.

   **Its first structural blocker is REMOVED: the epilogue is now keyed
   by CACHE STATE** (`exit_pc` interns `(cache, fcache, tflush)`,
   `emit_epilogues` emits one per distinct state). A single shared
   flushing epilogue can only write back a fragment-CONSTANT cache, so
   nothing per-instruction was expressible before this. Going back to
   an inlined per-exit tail is not an option - that is what blew the
   short jcc's displacement and grew every exit with the pool.

   That change also uncovered a latent bug (full record in
   docs/jit-optimizations.md): the barrier path emptied `cache` and
   `fcache` but not `tflush`, so with C3 elision active a barrier'd
   exit took the FLUSHING epilogue and wrote pre-call registers over
   the helper's writes. 57 of 108 corpus programs change; every changed
   line is a retargeted `jmp` or part of the new bare epilogue.

   ### ⛔ INTERVAL SHARING IS THE WRONG NEXT STEP - MEASURED, 2026-08-16

   The plan said to state the ceiling before writing code. Done, with
   **`MYLANG_REGAUDIT=1`** (pick_cached_slots), over bench/my + samples
   + tests/functional:

   | | |
   |---|---|
   | fragments | 127 |
   | qualified int candidates | 332 |
   | pinned today | 220 |
   | overflow (no register) | **112** |
   | overflow with an interval DISJOINT from some pin | 35 |
   | ...and EDGE-CLOSED (shareable with no per-edge fixup) | **0** |

   **Zero.** Letting two slots with disjoint live ranges share one
   register would give a register to no slot anywhere in the corpus.
   Two independent reasons, and the second is the general one:

   - The `8N_regs_int_*` family - the benches written FOR this task -
     report `shareable=0` outright. Every accumulator is updated every
     iteration, so their live ranges all span the whole loop and
     overlap completely. Sharing is meaningless there BY
     CONSTRUCTION, which is worth knowing before building it.
   - Where intervals ARE disjoint (68_nested: 41 candidates, 4 pinned,
     34 shareable) not one is edge-closed. Edge-closure demands that no
     jump cross the interval boundary, and a fragment with `if`s has
     forward branches over nearly every interior point. Only a
     whole-fragment interval survives - which is what we already have.

   Making sharing work therefore needs real **edge reconciliation**
   (moves inserted on control-flow edges, the thing a linear-scan
   allocator does), not the cheap edge-closed rule. That is a large
   machine for a benefit this measurement says is zero on today's code.
   **Do not build it without a corpus that wants it.**

   ### What the data DOES say: the pool is the binding constraint

   112 candidates have no register because there are only 6 registers
   and, on 83_regs_int_40, 42 candidates. So the question is not "use
   the registers better", it is "have more registers" - which is the
   maintainer's mandate restated. Ranked by how often the emitter
   hardcodes each as SCRATCH:

       RAX 297   RCX 165   RDX 133   RSI 84   RDI 76   r8 42   r9 14

   - **r9: DONE** (2026-08-16), pool 6 -> 7. Two local uses, both
     already safe under the gates r10/r11 rely on. Measured: loop
     instruction count BYTE-IDENTICAL (the scale3-minus-scale1 delta is
     328,000,022 on both sides), loop data references **-20.0% on
     80_regs_int_08**, wall clock 1.009x over the affected benches -
     the corrected cost model exactly. A prerequisite, not a win.
   - **rax/rcx/rdx/rdi: NOT reachable by adding a pool entry.** ~670
     hardcoded scratch uses; freeing them needs the emitter to ALLOCATE
     its scratch, which is the genuinely large piece of work left and
     the only route to 13.
   - **rsi/r8** (the type singletons): plan step 5 below, gated.
2. **Live ranges from `visit_use_def` - DONE.** `jit_slot_liveness`
   (codegen.h) answers the three questions an allocator asks: may a
   register be dropped without a write-back (is the slot dead), what
   must be written back at an exit (the live-out set), and may one
   register serve two slots (disjoint ranges). `jit_next_use` gives the
   spill heuristic its victim ranking - **deliberately a separate
   function, because it is a HEURISTIC**: pc order is not execution
   order across a back edge, and evicting the wrong register costs a
   reload rather than an answer. Nothing that must be correct may read
   it.

   The widening reuses `jit_fwd_info`'s fixpoint rather than adding a
   second one - both are now wrappers over `jit_liveness_core`, so
   `visit_use_def` / `visit_pc_fields` / the handler absorption are read
   from exactly one place. It also removes a real cliff: the old
   one-word mask gave up at `n_temps > 64` (`return false`), which
   silently downgraded C4a-i, C3 inc 3 and C5 to locals-only on any
   chunk that big. Emitted code byte-identical on all 108 corpus
   programs.

   ⛔ **AND THE TEST TAUGHT SOMETHING GENERAL, watched failing.** The
   obvious oracle - "compare the new all-slot analysis against the old
   temps-only one" - looked independent and WAS NOT, precisely because
   the widening made them share a core. Sabotaging the may-analysis
   direction (an unaudited op contributing NOTHING live, which is the
   direction whose loss is a silent miscompile) left that comparison
   perfectly green: the bug was on both sides and cancelled. **A test
   whose ORACLE SHARES THE IMPLEMENTATION UNDER TEST proves only that
   the shared part is self-consistent** - the same shape as "a test
   derived from a table can never find a hole in that table".

   What catches it is a check derived from the written CONTRACT instead:
   an op `visit_use_def` does not know must leave every covered slot
   live-in. That needed a program containing one of the 35 unaudited
   opcodes (an array element store - `StoreElemInt` is a barrier), and
   the test asserts it SAW one rather than passing vacuously - which it
   did on the first attempt, loudly.
3. **Spill the furthest next use** on pressure; reload on demand.
4. **Flush live registers at every exit. DONE - as a CHECKED CONTRACT,
   not an enumeration.** The note this replaces said "12 raw `u8(0xC3)`
   returns are NOT covered by `exit_pc` and must be enumerated". That
   count was STALE: the epilogue consolidation left exactly ONE `ret` in
   the emitter, inside `frag_ret()`. The hazard was real but differently
   shaped - **13 sites call `frag_ret()` directly**, bypassing
   `exit_pc`'s automatic flush, and only six of them flushed.

   The other seven rested on an invariant stated NOWHERE at those sites:
   `pick_cached_slots` lists neither `CallV` nor `CachedCallV` nor
   `CallValueV`, so they hit its `default: return {}` and a run holding
   a call caches nothing. **That is precisely the invariant this task
   exists to destroy** - the mandate is to spill live registers around a
   call and keep them across it.

   So the claim moved into the signature: `frag_ret(RetFlush)` with
   `flushed` (flush_cache() was emitted on this path), `empty`
   (ASSERTED - `cache/fcache/tflush` all empty here) and `epilogue`
   (emit_epilogues only, where exit_pc already chose per exit and the
   emitter's end-of-fragment state describes no particular exit).

   It ASSERTS rather than emitting the flush itself, deliberately:
   emitting would reorder the flush against the `mov rax, <sentinel>`
   the sites place first, and **byte-identical output is the cheapest
   proof the contract changed nothing** - verified 108/108 across
   bench/my + samples + tests/functional (normalising baked addresses
   and rel32 call displacements, which differ between two links; note
   40_math_builtins' libm displacement is 5 or 6 hex digits depending
   on ASLR, so the call rule must run BEFORE the width rule or the
   normaliser fights itself).

   WATCHED FAILING, and the sabotage is the exact future state: teach
   `pick_cached_slots` that a call run is cacheable and `-rt` aborts at
   the propagate/switch arms within seconds. **Honest scope: with
   `ASSERTS=0` that same sabotage keeps `corpus_diff` green (20/20).**
   So this is a guard for where #96 is going, not the fix for a live
   bug - and unlike the two xcache gates it does fire on a reachable
   shape, so it is not vacuous.

   The five `frag_ret` sites inside `emit_ret_native` are `flushed`,
   not `empty`: that function flushes on its FIRST line and every one
   of its returns inherits it. A "is there a flush_cache() within four
   lines above" heuristic mis-classified all five, and the assert
   caught it on the first run - which is the small version of the
   argument for having it.
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

## The test net that came out of the shift finding (2026-08-16)

`jit_fwd_family_coverage` - a ratchet over the specialized-arith opcode
RANGE rather than over the whitelist. It exists because the shift gap
survived four nets, each blind for a different structural reason (the
differential cannot see a speed-only change at all; the counter net asks
"did the lever run", not "for which opcodes"; the lever's own test was
written from the table it was meant to check; no corpus program had the
shape). Full account in docs/jit-optimizations.md.

**The transferable part, for the allocator work still ahead:** every
increment below adds or edits an opcode-keyed decision, and every one of
them fails SILENTLY when it goes stale. Before landing one, ask whether
its table has an enumeration a ratchet can walk - and if it does not,
build the enumeration first.

## The type SINGLETONS: why they must stop living in registers (2026-08-17)

**Maintainer's instruction:** *"constants should not be pinned to
registers, you should use immediate operands instead... Use immediate
operands as much as possible, they're fast and cheap."* Correct, and
the day's work proved it from the other side.

### What the register scheme costs, measured

`rsi` holds `t_int` and `r8` holds `t_float`. They are compile-time
CONSTANTS, yet they are re-materialised constantly, because both are
caller-saved: `emit_call_epilogue` re-does `movabs` after EVERY helper
call. Counted over the emitted code:

| bench | type stores via rsi | `movabs rsi` | `movabs r8` |
|---|---|---|---|
| 09_fib_recursive | 42 | **108** | **106** |
| 46_matrix_mult | 57 | **101** | **102** |
| 43_sieve | 31 | 48 | 46 |

**More materialisations than uses**, and on 09_fib_recursive - which
contains no float arithmetic at all - 106 `movabs r8` for a constant
nothing reads.

### And it made r8 UNSAFE to pin

Pinning r8 (d4b40c5) asked `run_has_float`, an OPTIMISTIC whitelist:
true for listed float ops, false for everything else. Used as a NEED
test it is unsound in the direction that matters, and it already was -
`UnpackElemValue` passes r8 as a helper ARGUMENT and needs the
singleton back, and is not in the list. So the pin gate got
"float-free", took r8, and the epilogue wrote `t_float` over the pinned
local. Nothing broke earlier only because the epilogue restored r8
UNCONDITIONALLY, masking it.

Fixed: ONE predicate, `run_needs_float_tag`, conservative (a run needs
the tag UNLESS every op is positively known not to touch r8), asked by
BOTH the pin gate and the tag materialisation - so `float_tag_live` and
`reg_holds_pin(8)` are mutually exclusive by construction. Plus
`emit_call_epilogue` never writes over a pin. `run_has_float` is
DELETED; a comment stands where it was.

**And the safe version costs the pin its whole benefit:**
80_regs_int_08 now pins r8 **zero** times, because its fragment is not
provably float-tag-free. The -40% data references r8 bought are gone.

### THE CONCLUSION, and it decides the next step

While the type tags live in registers, r8 cannot be freed usefully and
rsi cannot be freed at all - so **two of the thirteen registers are
unreachable by construction**, and ~100 instructions per program are
spent re-creating constants.

**The requirement, stated precisely, because it is easy to get wrong:**
`mov qword [rbx+disp], imm32` SIGN-EXTENDS its immediate, so what is
needed is a Type object at a LOW ABSOLUTE ADDRESS (< 2^31). "Near the
generated code" is a different property - it buys RIP-relative
addressing (`mov rax, [rip+d]; mov [slot], rax`), which is TWO
instructions and therefore worse than the pinned register. Only the low
absolute address gives the ONE-instruction store.

Today the binary is PIE and loads around `0x6348_0000_0000`, so no Type
pointer fits.

**The fix is to place the singletons ourselves.** They are ~12 objects,
constructed ONCE at startup, NEVER freed, a few hundred bytes total -
so the allocator this needs is a BUMP POINTER over one `mmap`ed region
(`MAP_32BIT` on Linux/x86-64, which is exactly where the JIT runs),
with placement-new for the Type objects and a fallback to normal static
storage anywhere else. No free list, no coalescing, no size classes, no
thread safety: none of what a general allocator provides is used.

The JIT then asks, per store, whether the pointer fits in an imm32 and
emits `mov qword [slot], imm32` when it does - one instruction, no
register, no entry materialisation, and no epilogue re-materialisation.
rsi and r8 both become ordinary pool registers, taking the pool from 8
to 10 with the two most-wasted instructions in the emitter deleted.

### The type tags on the OTHER backends - decided up front (2026-08-17)

Asked before building the x86-64 arena, so the arena is not designed
into a corner. **The answer is different IN KIND per ISA, and the
property that matters inverts.**

**AArch64 cannot use the x86-64 trick at all** - three facts, in
decreasing order of how badly they rule it out:

  1. there is **no store-immediate instruction**. `STR` always stores
     FROM a register, so `mov qword [slot], imm32` has no analogue and
     the tag must reach a register no matter what;
  2. an arbitrary 64-bit constant costs up to **4** instructions
     (`MOVZ` + 3x`MOVK`, 16 bits at a time);
  3. but PC-relative is cheap: `ADRP`+`ADD` reaches +/-4GB in **2**
     instructions, and `ADR` alone reaches +/-1MB in **ONE**.

**So on AArch64 the tags STAY in registers, and that is correct rather
than a concession.** What we are actually fighting on x86-64 is not
"constants in registers", it is constants in **CALLER-SAVED** registers:
rsi/r8 die at every helper call, which is the entire 108-and-106
`movabs` bill on 09_fib_recursive. AArch64 has **31 GPRs** and
**ten callee-saved** (x19-x28, AAPCS64), so the tags go in x27/x28,
are materialised ONCE at fragment entry, and a helper call cannot
touch them. The cost we are chasing simply does not arise.

| | x86-64 | AArch64 |
|---|---|---|
| scarce | callee-saved regs (6, we use 5) | nothing - 10 callee-saved |
| tag storage | low arena + `imm32` store, NO register | callee-saved x27/x28, set at entry |
| "near the code" buys | nothing (RIP-relative = a 2-instr load) | everything (`ADR`/`ADRP` are PC-relative) |
| Darwin | no MAP_32BIT + 4GB `__PAGEZERO` -> no arena | works, no arena needed |

**The shared thing is the POLICY** - the type tags are rematerialisable
CONSTANTS, never allocatable values - while the ENCODING is per-backend
behind one `emit_type_tag_store()` seam. If an arena is ever wanted on
ARM64, the useful property there is PROXIMITY to the code (so `ADR`
reaches it in one instruction), which is the exact opposite of x86-64's
"low ABSOLUTE address".

**Darwin x86-64** (Intel Macs) gets no arena either - no `MAP_32BIT`,
and a 4GB `__PAGEZERO` makes every low address unmappable. It falls
back to the register form, i.e. today's behaviour: correct, not
optimal, and there is no JIT there today anyway.

⛔ **BANK THIS NOW FOR DARWIN/ARM64, it is unrelated to constants and is
the thing most likely to be found late:** Apple Silicon enforces W^X on
JIT pages. The code buffer needs `MAP_JIT` plus
`pthread_jit_write_protect_np()` toggled around every write, and a
hardened process needs the `com.apple.security.cs.allow-jit`
entitlement. That is a constraint on the whole ARM64-on-Darwin backend,
not on this task.

### Step 3 ATTEMPTED and NOT LANDED - what it costs and where it stops

With the arena in (e652a13) every type tag is an immediate, so the
`movabs rsi`/`movabs r8` at each fragment entry and after each helper
call are, apparently, dead - and rsi/r8 look like free pool registers.
Both halves were tried. **Neither works yet, for the same reason.**

**(a) rsi CANNOT be pinned.** Adding it to `XCACHE_REGS` fails `-rt`
immediately. rsi is SysV ARGUMENT 2 and raw scratch at ~84 sites
(`mov rsi, rax`, `lea rsi, <slot>`), most of them OUTSIDE any
`emit_call_prologue` bracket, so a pin there is destroyed with nothing
to restore it.

**(b) DELETING the r8 materialisation breaks four float tests**, and
this is the more interesting one because it means my reader audit was
WRONG. I grepped for `cmp` against the tag registers, found only the
three `cmp_q_imm8(R8R, ...)` sites in emit_sync_push_native /
emit_ret_native (where r8 is scratch, not a tag), and concluded nothing
reads r8 as t_float. Something does. Failing:

    jit: C3 inc 3 float type-store elision admits safe TEMPS
    jit: C4b float literal registers + register arith sources
    jit: native container + bytecode island (model-flip M3)
    jit: nativized ops actually run natively (g_jit_op_calls bumps)

with the signature `flit ref-listed dst: got
[9670410800713881878528.00000]` - a garbage float, i.e. a value read
through a slot whose type word is wrong (or an r8 payload written where
a type was expected). Note it fails the same way WITH the conservative
pin gate restored, so it is the DELETION, not the pin.

**WHERE TO LOOK NEXT** (the audit to redo properly rather than by
grepping for `cmp`): the C4b float-literal path (`flit_load`,
`pick_float_lits`), `emit_put_scalar_call`, and any site that writes a
type field through a register the six converted ones did not cover.
The right method is not another grep - it is to instrument
`store_type_tag` and the r8 writers, or to diff `-vdj` for a failing
program with and without the deletion and read what changed.

⛔ **THE FINDING THAT MATTERS, AND IT IS THE PLAN-LEVEL ONE: freeing a
register from a CONSTANT is NECESSARY BUT NOT SUFFICIENT.** rsi and r8
each had TWO jobs - the type singleton AND an argument/scratch role -
and the arena only removed the first. `run_needs_float_tag` was
additionally, and silently, excluding the runs where r8 is used as an
argument, which is why r8-as-a-pin appeared to work at all.

So the remaining registers (rsi, r8, rax, rcx, rdx, rdi) are ALL
blocked on the same thing: **the emitter must allocate its scratch.**
That is the one large piece left in #96, and steps 1-2 were the
prerequisite for it, not a substitute.

### Step 3 LANDED (2026-08-17) - and what it did NOT buy

Recovered from the session transcript after I destroyed the working
tree with `git checkout --`; see the standing rule now in the per-user
CLAUDE.md. The lost hour was replayable only by luck.

DONE: the tag materialisations are gone on Linux x86-64 (`movabs`
-31.8%, all emitted instructions -6.72% corpus-wide, code ~6% smaller);
`cmp_reg_tag` is the read seam; `jit_xcache_busy` replaced the prefix
count with a per-register mask. -rt 1921/1921, four differentials,
corpus_diff plain/--levers/--cold, norec_enum depth 3 all green.
Wall clock flat (suite 1.006x) - banked as a prerequisite, per the
cost model.

**THE BLOCKING FACT, now measured rather than suspected:** freeing a
register from a CONSTANT does not free the REGISTER.

    rsi  = t_int   AND SysV arg 2 AND ~84 raw scratch sites
    r8   = t_float AND SysV arg 5 AND ~20 raw scratch sites
    rax 297   rcx 165   rdx 133   rdi 76   (scratch mentions)

`run_needs_float_tag` had been quietly doubling as r8's exclusion for
the argument/scratch role, which is why r8-as-a-pin ever appeared to
work. Adding rsi to XCACHE_REGS fails -rt at once.

**SO THE NEXT STEP IS THE EMITTER'S SCRATCH ALLOCATION, and it is the
whole remaining piece of #96.** Sketch, to be argued before building:

 1. give the Emitter a scratch REQUEST api (`take_scratch()` /
    `scoped_scratch`), initially satisfied by exactly the registers
    hardcoded today, so the change is provably byte-identical
    (`scripts/vdjcmp.sh` over the corpus is the oracle);
 2. convert the ~670 sites family by family, each conversion
    byte-identical, never a behaviour change bundled with a refactor
    (see [[bundled-change-needs-a-kill-switch]]);
 3. only then let the pool draw from the freed set, one register at a
    time, each with its own measurement.

Step 1-2 are pure restructuring and can be verified absolutely; step 3
is where the 13 registers actually arrive.

### The scratch census, CORRECTED (2026-08-17) - and it names a different
### first target than every previous note in this file

`scripts/regcensus.py`. Two mistakes in the earlier counts, each of
which pointed at the wrong register:

 1. **comments and strings are not code.** `grep -c RSI src/jit.cpp`
    said 92 - and a large part of that was prose ABOUT rsi, including a
    stale scratch-count comment I had written myself. Scrubbed: 85, and
    the *unbracketed* part is 23.
 2. **a use inside a call bracket is already safe.** `emit_call_prologue`
    SPILLS every caller-saved pin and the epilogue reloads it, so SysV
    argument setup between them cannot disturb a pin whatever register
    it writes. Counting all uses is counting the calling convention.
    (The bracket scan must itself run on scrubbed text: comments
    mentioning `emit_call_prologue` opened brackets that never closed,
    which alone moved RAX from 8 "bracketed" to 190.)

    reg    bracketed   UNbracketed   total   cost to free
    RAX            8           254     262   expensive
    RCX           36           135     171   expensive
    RDX           57            76     133   mid
    RSI           62            23      85   mid
    RDI           59            14      73   CHEAP
    r8            0            42      42   mid
    r9            0            14      14   CHEAP  (already pooled)

**So "~670 sites" was never the real number, and rsi/rdi are the
CHEAPEST registers to free, not the hardest.** The claim in the step-3
commit that "rsi is SysV arg 2 plus ~84 raw scratch sites" is WRONG:
it is arg 2 plus **23** unbracketed sites, several of which are the
no-arena fallback (`store_type_tag(..., RSI)`, `cmp_rax_tag(..., RSI)`,
the tag materialisation) that emit nothing on Linux at all.

**THE STAGED PLAN, cheapest first, each increment independently
verifiable:**

 - **inc 1 - RDI (14 sites).** Read them first: several are ABI arg-1
   setup whose bracket the scan cannot see (`lea_rdi` inside a helper
   call sequence), so the true count is lower still. Convert the
   genuine scratch uses to a tracked request, leave the ABI ones.
 - **inc 2 - RSI (23, minus the no-arena fallbacks).**
 - **inc 3 - RDX (76).**
 - **inc 4/5 - RCX (135) and RAX (254)**, the two that need the real
   mechanism rather than a hand conversion.

**THE MECHANISM, and why it is not just renaming.** What blocks the pin
allocator is not that a site names RAX - it is that the emitter keeps
no record of which scratch registers are LIVE at a point, so the
allocator cannot know what is free. So each increment adds a tracked
request (`scratch_busy` mask + an RAII holder + an `ML_CHECK` that a
requested register is not currently holding a pin - the invariant
`jit_assert_no_volatile_pin` checks by hand today), while still handing
back the SAME register, so emission stays byte-identical.

**THE ORACLE FOR EVERY INCREMENT IS `scripts/vdjcmp.sh`**: emitted code
byte-identical across the corpus. A conversion that changes one byte is
not a conversion, it is an optimization wearing one - and those get
measured separately ([[bundled-change-needs-a-kill-switch]]).
Re-run `scripts/regcensus.py` after each one; the UNbracketed column is
the burn-down.

### inc 1 analysis: the sites fall into THREE kinds, and only one is work

Reading all 14 RDI sites (`scripts/regcensus.py RDI`) changes the shape
of the remaining arc. They are not 14 instances of one problem:

**(a) SysV setup inside an `emit_call_prologue`/`epilogue` bracket.**
Already safe for ANY caller-saved register: the prologue spills every
caller-saved pin and the epilogue reloads it. Nothing to do, ever.

**(b) SysV setup in a HAND-ROLLED call sequence** - `push_reg(RDX)`,
`sub rsp,8`, load the args, `call` (the M5c cache probe at 3708, the
borrow-bind at 4315, `jit_stage_args` at 4866, the sync-call fragment
entry at 4347/12332 where `rdi` is the JIT's OWN fragment ABI). These
do NOT spill pins - and they do not have to, because the whole RUN is
denied caller-saved pins by **`jit_run_blocks_xcache`**, and
**`jit_assert_no_volatile_pin`** ASSERTS that at both emitters. The
mechanism, the opcode list AND its staleness net already exist.

**(c) genuine scratch inside a run** - the only real blocker. For RDI
that is FOUR sites: `load_operand(e, RDI, ...)` (8468, 8571) and
`e.load(RDI, val.payload)` feeding `store_elem_byte_dil` (9172, 9214),
which is `mov [rcx+r9], dil` and therefore wants RDI's low byte
specifically.

**SO THE REMAINING WORK IS NOT "CONVERT 670 SITES TO AN ALLOCATOR".**
It is, per register: find its kind-(c) sites, and for each either move
it to another register or make its OPCODE block that register's pin.

**THE DESIGN THAT FALLS OUT: per-register block lists.** Today
`jit_run_blocks_xcache` is one global "this run gets NO caller-saved
pin" gate - correct but maximally coarse, because it is driven by the
worst op in the run. Replace it with a per-opcode CLOBBER MASK (which
registers this op writes behind the allocator's back); a register may
be pinned in any run whose ops do not name it. Then:
  - kind (a) contributes nothing (bracketed);
  - kind (b) contributes "all caller-saved", exactly today's behaviour;
  - kind (c) contributes just the register it really uses.
`jit_assert_no_volatile_pin` generalises to "no pin is live in a
register this op's mask claims", which keeps the existing staleness net
and makes a forgotten mask entry a named abort rather than a silent
corruption.

That is a much smaller and much more honest piece of work than a
general scratch allocator, and it is the thing that actually unblocks
registers. A real allocator only becomes necessary for RAX/RCX, whose
kind-(c) counts (254/135) are large enough that per-op masks would
block nearly every run.

**NEXT ACTION (SUPERSEDED - read the section below first).**

---

## 2026-08-17: the census was WRONG and r9 was a shipping bug

The (a)/(b)/(c) taxonomy above is CONFIRMED - it was re-derived against
a corrected census and holds for every register examined. Two things
around it were wrong.

**1. THE SITE COUNTS WERE LOW BY UP TO 6x.** `regcensus.py` could not
see an accessor whose NAME encodes the register (`lea_rdi`,
`slots_to_arg0`, `store_elem_byte_dil`, `movabs_r9`, `cmp_r9_rdx`) -
CLAUDE.md's sixth audit-table shape, hit by the tool written to avoid
it. It now derives the accessor set from the source. Corrected
unbracketed counts: **RAX 392, RCX 193, RDX 142, R9 87, R8 45, RDI 33,
RSI 23** (was 254/135/76/14/42/14/23).

**2. r9 WAS NEVER SAFE, AND SHIPPED A WRONG ANSWER FOR A DAY.** It is
raw scratch in the capture ops and every element tier. Removed from
`XCACHE_ORDER`; full record in docs/jit-optimizations.md
("#96 - r9 was NEVER safe as a pin"). Three nets came out of it:
`MYLANG_JIT_XROT=N` (rotate the pool so its TAIL gets first-choice
traffic), `Emitter::scratch(reg)` (declare raw-scratch use at the site;
ML_CHECK that no pin lives there), and a REPAIRED `vdjcmp.sh` (it had
been reporting 0-identical for every comparison since #96 step 3, and
77/31 against itself).

### The RDI increment, re-derived and still valid

RDI's 33 unbracketed sites split exactly as the taxonomy predicts:
 - **(a)/(b)** 12 sites - `emit_sync_push_native` (3),
   `emit_sync_call_inline` (3), the `CallV` case (1), all blocked
   because the run contains a call; `emit_ret_native` (5), whose first
   act is `flush_cache()`;
 - **(c)** 21 sites in just TWO emitters - `emit_store_elem_inline`
   (13) and `emit_store_elem2_inline` (8) - reached by exactly THREE
   opcodes: `StoreElemInt`, `StoreElemFloat`, `StoreElem2V`.

So the clobber mask for RDI is three opcodes, and both emitters already
declare `e.scratch2(9, RDI)`, so a mistake aborts by name.

### ⛔ BUT: DO NOT DO IT NEXT. The measurement says it will not pay.

The r8 entry in docs/jit-optimizations.md already measured this arc's
central result - **MORE REGISTERS DO NOT PAY HERE** - and RDI would be
the pool's 4th caller-saved member, available only in a run with no
call, no C1 hoist region AND no element store. That is a narrow set,
and it buys at most one more pinned local in it.

Adding it would be another opportunistic pool widening justified by
"the site count is small", which is the exact reasoning that produced
the r9 bug. The site count is a COST estimate; it was never evidence of
a BENEFIT.

**THE HONEST NEXT STEP IS THE SCRATCH ALLOCATOR ITSELF**, because that
is what the maintainer actually asked for ("all the sites need to use
the allocator instead of hard-coding the registers") and it is the only
route to RAX/RCX/RDX - 727 of the 915 unbracketed sites, and the only
registers numerous enough for the outcome to matter. Suggested shape,
smallest-first:

 1. `Emitter::alloc_scratch(n)` / `free_scratch()` - hand out registers
    that hold no pin and no live emitter state, ML_CHECKing exhaustion.
    `scratch(reg)` (already landed) is the read-only half of this
    contract and its call sites become the first conversions.
 2. Convert ONE emitter family end-to-end (`emit_store_elem_inline` +
    `emit_store_elem2_inline` - 21 RDI sites and 20 r9 sites, and they
    already declare their scratch). Oracle: `vdjcmp.sh` must stay
    108/108 identical when the allocator happens to choose the same
    registers, and `-rt` + `--xrot` must be green when it does not.
 3. Only then reconsider the pool, WITH a measurement.

## 2026-08-18: the ScratchPlan lands, and MEASURES ITSELF UNREACHABLE

The store-elem emitters no longer name registers. `ElemScratch` gives
the tier five ROLES - obj / data / count / idx / val - and
`elem_scratch_plan()` allocates `idx` and `val`, declining to the helper
if nothing is free. 149 hardcoded references became role names, and
`vdjcmp.sh` proves the emitted code byte-identical.

**AND THEN IT MEASURES ITSELF UNREACHABLE, WHICH IS THE FINDING.**

Instrumented over the whole corpus WITH rdi added to the pin pool, the
plan is consulted **58 times and never once with rdi or r9 pinned**.
The pin count at every one of those 58 calls is at most 4 - exactly
`MAX_CACHED`, the callee-saved four. No caller-saved pin is ever live
when the element tier emits.

**The cause is structural, not luck:**

    xcache_ok = hregs.empty() && !jit_run_blocks_xcache(..) && !lever

An element store on a loop-invariant base is precisely what makes
`jit_hoist_pick` return a region, so `hregs` is non-empty exactly when
the element tier fires - and the gate then denies the run EVERY
caller-saved pin. The element tier and the caller-saved pool are
mutually exclusive BY CONSTRUCTION.

So: **freeing rdi and r9 inside the element tier buys nothing today**,
and would have bought nothing however many encoders were converted.
Converting them was still right - the roles are the only way an
allocator can ever exist there, and the conversion is what made this
measurable at all - but the next increment is NOT more of it.

### The next increment, redirected (again)

**Make the C1 hoist mark only the registers it actually uses.** The
gate is maximally coarse: a hoist region uses r10 and r11, and denies
r8 (and any future member) for no reason. Replacing the boolean with
the per-register mask already designed above - hoist contributes
{r10, r11}, a call contributes all caller-saved, an element store
contributes {} once its scratch is allocated - is what lets a pin and
an element store coexist. Only then does the ScratchPlan relocate, and
only then is "add rdi to the pool" a question worth measuring.

Watched: with rdi forced into the pool the plan still never relocates,
and 5 corpus programs change only because the PIN moved into rdi
elsewhere (helper counts identical, so no tier declined) - which is
also the evidence that rdi is now safe to pin whenever the gate allows
it.

## 2026-08-18: the gate becomes a mask - and the arc's answer repeats

Done, in three commits: the no-arena lever + the bool-tag wrong answer
it found; the clobber mask; the float-tag narrowing. Full record in
docs/jit-optimizations.md.

**REACHABILITY, which is the number that matters here:** runs with a C1
hoist region that have a caller-saved register left went **0 -> 20 of
20** (corpus-wide, 199 runs compiled). The mechanism works and is
proven to run.

**THE MEASUREMENT: flat.** 9 corpus programs change; Ir -0.71% ..
+0.22%; wall-clock geomean cur/base **0.997x** interleaved. That is the
THIRD time this arc has answered "more registers do not pay here" (r8,
then the ScratchPlan, now the mask). Treat it as settled: **pin
PRESSURE is not what these programs are short of.** Do not open another
increment whose thesis is "one more pinnable register".

### What this DID buy, and it is not speed

 - the gate states a FACT per register instead of one coarse
   approximation, so a future pool member is denied only by a
   contributor that names it;
 - `jit_xcache_busy` no longer claims r8 for a singleton that is not in
   a register - a claim that was simply FALSE on the shipping path, and
   was out of step with `elem_reg_usable`, which already had it right;
 - a `size_t` underflow in the C2b pair pick (`MAX_CACHED -
   hot.size()`), latent behind the very coupling the mask removes, is
   gone;
 - the no-arena configuration is testable at all, which found a
   shipped wrong answer in its first run.

### The next increment is NOT here

The register file is not the constraint. Task #97 - the CALL overhead,
which owns 7 of the 8 worst my/cpp benches - is where the time is. If
this task is resumed, resume it for the RAX/RCX/RDX scratch allocator
(727 of 915 unbracketed sites) with a measurement in hand FIRST showing
what a shortage of those costs, not on a site count.

## 2026-08-18 (b): MYLANG_JIT_MAXPINS, and what a pin is actually WORTH

Three increments widened the pool and all three measured flat, which
left the important question unanswered: does that mean "the pool is
already big enough", or "pinning does not pay on these shapes at all"?
`MYLANG_JIT_MAXPINS=N` caps the budget so the difference between N and
N+1 IS the marginal value of one register. Self-test: a non-binding cap
must be a no-op - verified byte-identical on 108/108 corpus programs.

**The sweep (callgrind Ir, OPT=1 ASSERTS=0):**

    bench              where the win is        beyond it
    07_nested_loops    pin 1   -5.36%          pins 2..7  +0.00%
    01_while_loop      pin 2   -5.85%          pins 3..7  +0.00%
    80_regs_int_08     -                       EVERY pin  +0.00%
    83_regs_int_40     -                       EVERY pin  +0.00%

**The whole value of the register cache is the first one or two pins**,
and on the two benches written expressly to stress register pressure -
8 and 40 hot int locals, independent recurrences, the throughput regime
- pinning is worth NOTHING AT ALL.

Two hypotheses tested and REJECTED:
 - *lever A already captured it.* No: the numbers are the same with
   `MYLANG_JIT_OFF=fwd` (83: +0.00% either way; 07: -4.84% vs -5.36%).
 - *the pick declines them.* No: each accumulator is read/written ~5x
   per iteration, far over the >= 3 threshold, and 6 do get pinned
   (`xcache 1` in JITSTATS confirms a caller-saved entry).

And the direct evidence: on 83_regs_int_40 the fragment is **1367
emitted instructions at N=0 and 1416 at N=7**. Pinning makes the code
BIGGER and leaves the dynamic count identical.

### What this means for the task

**The register FILE is not the constraint, and neither is the pool
SIZE. Something downstream is eating the pin's benefit before it
reaches the loop body, and until that is found, every additional
register is worth measured zero.** That is a different question from
the one the last three increments answered, and it is the one worth
answering next - it is also the only thing that could make "13
registers" pay.

FIND IT FIRST, cheaply: take 83_regs_int_40's hot fragment, dump it at
N=0 and N=7, and account for every instruction the pin was supposed to
remove. The candidates, in the order they are worth checking:
 1. the accumulator's uses may not be CACHE-AWARE - a `bad()` site, or
    an op whose helper must see memory, so the slot is pinned and read
    from memory anyway (CLAUDE.md's "WHICH bad() SITES ARE WORTH
    REMOVING" already records this class and says the second kind
    cannot be fixed);
 2. the run may be SPLIT so the loop body is several fragments, each
    paying entry/exit for pins it uses a handful of times;
 3. a barrier inside the loop may be flushing and reloading the whole
    cache per iteration.
Only (1) and (3) would be fixable, and both are cheap to rule in or out
from one `-vdj` dump.

⛔ **Do NOT widen the pool again before that accounting exists.** It has
been measured flat three times; a fourth is not evidence, it is a habit.

## 2026-08-18 (c): WHY the pins deliver zero on 83_regs_int_40

Answered, from the emitted code. **The pin changes each operand's
ADDRESSING MODE and not the instruction COUNT.** Same source line
(`a0 = a0 + i; a0 = a0 ^ (a0 >> 3);`), same fragment, two caps:

    cap=0  (a0, i in memory)      cap=7  (a0->r14, i->r13)
    mov rax, a0                   mov rax, r14
    mov rcx, i                    mov rcx, r13
    add rax, rcx                  add rax, rcx
    mov a0, rax                   mov r14, rax
    sar rax, 3                    sar rax, 3
    mov rcx, a0                   mov rcx, r14
    xor rax, rcx                  xor rax, rcx
    mov a0, rax                   mov r14, rax
    ---- 8 instructions ----      ---- 8 instructions ----

**The whole loop body is 340 instructions per iteration AT BOTH CAPS**,
and 209 of the 340 are `mov`. The real work is 122 (42 add, 40 xor, 40
sar). Pinning converts memory movs into register movs one-for-one; the
count cannot move, which is exactly what callgrind reported.

THE CAUSE: the B1/B2 specialized arithmetic emitter is a fixed
THREE-ADDRESS-THROUGH-THE-ACCUMULATOR shape - materialise operand 1 in
RAX, operand 2 in RCX, apply the op, store RAX to the destination. A
register a pin owns can be the SOURCE of the `mov`, but it can never BE
the operand of the `add`. So a pin removes a load's LATENCY (real, but
invisible to Ir and small next to 40-way ILP) and removes no
instruction at all.

Two things fall out, and the second is the one that pays:

**(A) TWO-ADDRESS ARITHMETIC when dst is one of the sources.** Every
accumulator in this family has that shape (`a = a + i`, `a = a ^ e`),
and so does every `+=` in the language. x86 does it in ONE instruction:

    pinned dst    add r14, r13                       4 -> 1
    memory dst    mov rax, i ; add [rbx+d], rax      4 -> 2

Note the memory form needs NO pin: `add r/m64, r64` is a legal
encoding, so this pays on the 33 accumulators that will never get a
register. That is the half worth doing first, and it is also what makes
a pin worth having at last - with two-address arithmetic in place a
pinned accumulator is 1 instruction against memory's 2, which is a
REASON to widen the pool that we have never had before.
Constraint to check: the destination's TYPE TAG. `add [rbx+d], rax`
writes the payload only, so it composes with C3's tag elision and needs
that elision to be in force (or one extra tag store, still 3 < 4).

**(B) IMMEDIATE OPERANDS for small constants.** The loop counter emits

    mov rax, r13 ; movabs rcx, 1 ; add rax, rcx ; mov r13, rax

where `add r13, 1` is one instruction. `movabs` for a value that fits
imm8 is the same waste the type-tag arena removed, one level down -
and the maintainer already named it: *"constants should not be pinned
to registers, use immediate operands"*.

### So the order is now clear, and it INVERTS the task

**The pool was never the constraint; the OPERAND ROUTING is.** Fixing
it is worth ~47% of this loop body on its own, most of it without any
pin at all - and it is the precondition that finally gives an extra
register something to do. Widening the pool first was, in hindsight,
optimising the wrong end: three increments moved which register held a
value and none of them could change how many instructions moved it.

Suggested increments, smallest first, each measurable on its own:
 1. two-address form for a MEMORY destination (`add [rbx+d], rax`) -
    no pin interaction, biggest reach;
 2. two-address form for a PINNED destination (`add r14, r13`);
 3. imm8/imm32 operands for small constants in the RI family;
 4. only THEN re-measure the marginal value of a register with
    MYLANG_JIT_MAXPINS. It should be non-zero for the first time.


## 2026-08-18 (d): increment 1 LANDED - memory two-address arithmetic

`<op> [rbx+d], reg` for `dst = dst OP b`. Ir: **83_regs_int_40 -17.05%,
82 -15.06%, 81 -11.30%, 80 -5.92%**, everything else flat. Fragment
817 -> 747 instructions. Full record + the five conditions (three
soundness, two cost, each sabotage-tested) in docs/jit-optimizations.md.

**WALL CLOCK FLAT (geomean 1.005x), and the reason is mechanical:** an
x86 RMW decodes to the same micro-ops as the load/op/store it replaces
(3 either way). Callgrind counts instructions, not uops. So increment 1
buys code size and decode slots, not time.

### That REORDERS the remaining increments

Increment 2 (`add r14, r13`, a PINNED dst) is now the one expected to
pay in time, and it is INDEPENDENT of increment 1 rather than built on
it: four instructions - of which the two register moves are eliminated
at rename but still occupy decode slots - collapse to ONE uop. That is
a real reduction in work, not a re-encoding of it.

 2. two-address form for a PINNED destination      <- do this next
 3. imm8/imm32 operands for small constants in the RI family
    (`add r13, 1` for the loop counter, vs
     `mov rax,r13; movabs rcx,1; add rax,rcx; mov r13,rax`)
 4. re-measure the marginal value of a register with MYLANG_JIT_MAXPINS

And note what increment 2 does to the ARC's central question: a pinned
accumulator would become 1 uop where memory is 3, which is the first
mechanism in this whole task that makes an ADDITIONAL register worth
something measurable. The 13-register goal gets its justification from
increment 2, not from any pool widening.


## 2026-08-18 (e): increment 2 LANDED - and the arc finally pays

`<op> pin, <src>`. `add r14, r13` for four instructions. Full record in
docs/jit-optimizations.md; the headline:

    Ir   01_while_loop -37.35%, 80 -27.13%, 81 -24.79%, 82 -23.16%,
         83 -22.29%, 07_nested_loops -11.34%, 43_sieve -6.88%
    WALL geomean cur/base 0.990x, 01_while_loop 0.62x, 81 0.86x

**THE FIRST WALL-CLOCK WIN OF #96**, and it lands exactly where the
micro-op argument said it would: the memory half re-encodes work (RMW =
same uops), the register half removes it (two renamed moves and a store
collapse into one uop).

### The arc, honestly summarised

Three increments widened the PIN POOL and measured flat, because the
pool was never the constraint - the emitter could not name a pinned
register as an arithmetic OPERAND, so a pin only changed an addressing
mode. Two increments fixed the OPERAND ROUTING and the same programs
moved 22-37%. The register file mattered all along; what was missing
was an instruction shape that could use it.

### What is left

 3. imm operands beyond the two-address path - the loop counter still
    emits `mov rax,r13; movabs rcx,1; add rax,rcx; mov r13,rax` when it
    lowers to ForLoopStep/IntAddStep rather than IntAddRI, which this
    increment does not touch. (`op_reg_imm` already prefers imm8 for
    the paths it does reach.)
 4. RE-MEASURE the marginal value of a register with
    MYLANG_JIT_MAXPINS. It should be non-zero for the first time - a
    pinned accumulator is now 1 uop where a memory one is 3 - and THAT
    number is what should decide whether to widen the pool toward 13.
    Do not widen it before running the sweep.
 5. The cost guards declined in increment 1 (fa, ref-listed dst) are
    still declined. Revisit WITH a measurement.

Also queued from this session (maintainer-raised, both AFTER the
allocator): #100 strength-reduce constant MULTIPLICATION, #101 a native
peephole + scheduling pass run last and agnostic of everything else.


## 2026-08-18 (f): the sweep re-run - THE POOL IS NOW MEASURABLY TOO SMALL

    WHOLE POOL (cap 0 -> 7)     before      now
    83_regs_int_40              +0.00%     -3.19%
    80_regs_int_08              +0.00%    -12.44%
    01_while_loop               -5.85%    -16.57%
    07_nested_loops             -5.36%    -11.33%

    MARGINAL, per additional pin
    83_regs_int_40   pins 3..7  -0.76 -0.61 -0.62 -0.62 -0.62 %
    80_regs_int_08   pins 3..7  -2.96 -2.44 -2.50 -2.57 -2.64 %
    01_while_loop    pins 1,2   -8.28 -9.04 %, then flat (2 hot locals)
    07_nested_loops  pins 1,2   -8.50 -3.09 %, then flat

**The curve does NOT plateau at 7 on the pressure benches, and 7 is the
entire pool.** Each further register is worth a steady -0.62% (83) /
-2.6% (80). 7 -> 13 on 83 projects to about another -3.7%.

So the original mandate is now backed by a measurement rather than a
site count, and the order it should be done in is settled:

 - FIRST the operand routing (done: increments 1 and 2). Without it a
   register cannot pay, which is what the three flat increments were
   really telling us.
 - THEN widen the pool. The remaining cost is the SCRATCH ALLOCATOR for
   RAX/RCX/RDX (717 of 1016 unbracketed sites) - the same work the plan
   named months ago, but now with a per-register return attached to it
   instead of a hope.

Still open, cheaper, and worth doing before the big conversion:
 - increment 3: the loop counter still emits
   `mov rax,r13; movabs rcx,1; add rax,rcx; mov r13,rax` where it
   lowers to ForLoopStep/IntAddStep rather than IntAddRI, which
   increment 2 does not reach;
 - the increment-1 cost declines (fa, ref-listed dst), revisit WITH a
   measurement;
 - #100 (constant multiply strength reduction) and #101 (the native
   peephole pass), both maintainer-raised, both after the allocator.


## 2026-08-18 (g): increment 3 LANDED - the counted-loop step

    inc r13 ; cmp r13, n ; jl        (was 7 instructions, now 3)

Ir cumulative over 1+2+3: 01_while_loop -37.35%, 80_regs_int_08
-31.07%, 83_regs_int_40 -23.26%, 43_sieve -12.08%, 07_nested_loops
-11.35%, 44_primes_sqrt -6.13%, 03_int_arith -5.50%.
Wall: geomean **0.985x** (0.990x after 1+2), 01_while_loop 0.64x,
81_regs_int_14 0.80x.

`inc`/`dec` used here on the maintainer's suggestion; safe SPECIFICALLY
because the flags are dead (the cmp on the next line resets them) and
because IntAddStep already used the idiom. Not adopted generally - the
imm8 form is one byte larger and writes flags completely.

### Remaining, in order

 1. **IntAddStep**, the same conversion - its accumulate is still
    read/load/op/write and its bound test still routes through RAX.
    Deliberately left out so increment 3 measured alone.
 2. The increment-1 cost declines (fa, ref-listed dst), WITH a
    measurement.
 3. **Widen the pool toward 13** - now justified by the re-run sweep
    (each register -0.62% on 83, -2.6% on 80, not plateauing at 7).
    The cost is the scratch allocator for RAX/RCX/RDX, 717 of 1016
    unbracketed sites.
 4. #100 (constant multiply strength reduction), #101 (native peephole
    pass) - both maintainer-raised, both after the allocator.


## 2026-08-18 (h): IntAddStep converted - the operand-routing arc closes

Its two halves are INDEPENDENT (accumulator and counter are different
slots), so each converts on its own and the boxed code still serves
whichever half is memory-resident.

    Ir cumulative (1+2+3+3b): 01_while_loop -37.35%, 80 -31.07%,
    83 -23.26%, 14_array_subscript -12.69%, 43_sieve -12.08%,
    07_nested_loops -11.35%, 44_primes_sqrt -6.13%, 18_foreach -5.72%
    WALL geomean 0.978x (was 0.985x, 0.990x, 1.005x, 1.000x)

The suite geomean over the whole arc: 1.005x -> 0.990x -> 0.985x ->
**0.978x**. Two of the benches above are NEW to the arc
(14_array_subscript, 18_foreach_array): they are the `sum += a[i]`
shape, which only IntAddStep reaches.

### The operand routing is now done. What remains is the POOL.

 1. **Widen the pool toward 13** - the re-run MAXPINS sweep justifies
    it with a number (-0.62%/register on 83, -2.6% on 80, no plateau
    at 7, which is the whole pool). Cost: the scratch allocator for
    RAX/RCX/RDX, 717 of 1016 unbracketed sites. THIS is the next big
    piece and it now has a mechanism behind it.
 2. Re-run the MAXPINS sweep AGAIN after 3b - the per-register value
    should have risen again, since more of the loop body is now
    register-addressable.
 3. The increment-1 cost declines (fa, ref-listed dst), WITH a
    measurement.
 4. #100 (constant multiply strength reduction), #101 (native peephole
    pass) - both after the allocator.


## 2026-08-18 (i): the sweep, third run - GO on widening the pool

    WHOLE POOL (cap 0 -> 7)   pre-arc   after 1+2      now
    83_regs_int_40             +0.00%      -3.19%   -4.11%
    80_regs_int_08             +0.00%     -12.44%  -16.19%
    01_while_loop              -5.85%     -16.57%  -16.57%
    07_nested_loops            -5.36%     -11.33%  -11.34%
    14_array_subscript              -           -  -24.93%

    MARGINAL at the pool's edge:  83 -0.63%/pin,  80 -2.78%/pin
    and 80's is RISING (-2.57 -2.64 -2.71 -2.78 for pins 4..7).

**No plateau at 7 on the pressure benches, and the per-register value
is now going UP with each pin.** The plateaus that DO appear land
exactly at each program's hot-local count (01 at 2, 07 at 2, 14 at 3),
which is the cross-check that the curve is real.

**PROJECTION: 7 -> 13 is worth ~-5.6% on 80_regs_int_08 (it has 9 hot
slots, so 13 captures all of them) and ~-3.8% on 83_regs_int_40.**

### THE DECISION THIS SETTLES

Widening the pool is now the right next step, for the first time in
this task, and for a reason rather than a site count. The cost is the
SCRATCH ALLOCATOR for RAX/RCX/RDX - 717 of 1016 unbracketed sites - and
the shape is already proven by the ScratchPlan (roles, not register
names) built for the element tier.

Order for that work, smallest-first, each with vdjcmp as the oracle:
 1. `Emitter::alloc_scratch(n)` / `free_scratch()` over the register
    STATE, satisfied initially by exactly today's hardcoded choices so
    the emitted code is byte-identical;
 2. convert ONE family end-to-end and prove it byte-identical;
 3. admit ONE register to the pool and re-run this sweep. Repeat.
Never admit a register by analogy - r9 shipped a wrong answer that way.


## 2026-08-18 (j): the scratch allocator - the SEAM, and why the rest is
## one indivisible conversion

`Emitter::alloc_scratch()` / `free_scratch()` land: hand out a register
holding no pin and no live state, ML_CHECK on exhaustion, preference
order rax,rcx,rdx so a converted site gets what it used to hardcode and
the emitted code stays byte-identical (verified 108/108 against the
previous commit).

**AND THE MEASUREMENT SAYS THE REST CANNOT BE INCREMENTAL.** Probe over
bench/my + samples + tests/functional: of **199 compiled runs, ZERO**
avoid the ops that use rax/rcx/rdx as raw scratch. Top blockers by op
count: CallBuiltinV 384, LoadConstV 251, LoadBuiltinV 178, ArrLen 138,
Halt 126, StoreGlobalV 123, MakeClosureV 119, ReturnV 101, LoadElemInt
101, SubscriptV 96 - a long tail, not a handful.

Two structural reasons, and neither yields to picking a subset:
 - a FRAGMENT IS A WHOLE FUNCTION BODY (`native_leaf`), so it always
   contains a return, a store, a load and usually a call;
 - `read_slot` / `write_slot` / `store_dst` - which EVERY op goes
   through - take RAX by caller convention.
So admitting rax to the pool while ONE site still writes it unasked is
a silent wrong answer of exactly the r9 shape. The conversion must be
COMPLETE before the pool changes. Census is the progress bar:
**RAX 399, RCX 222, RDX 118** unbracketed sites.

⛔ NOTE a helper CALL is NOT a blocker. `emit_call_prologue` already
spills every caller-saved pin - that is what made r8/r10/r11
admissible. Only RAW scratch outside that bracket is.

### THE PREREQUISITE NOBODY HAD NOTED: lever A's protocol names RAX

`JitFwd::in_rax` - the field name, and 14 sites - IS the forwarding
ABI: a producer leaves its value in RAX and the consumer reads it
there. **RAX cannot become an allocatable scratch register until that
carries WHICH register holds the value.**

The fix is already precedented IN THE SAME STRUCT: the float twin was
converted for exactly this reason ("C4b inc 2: the register is no
longer always xmm0, so it is carried"). So the int side follows a shape
that already exists and is already tested.

### ORDER, revised by the above

 1. **Generalise `JitFwd::in_rax` to `in_reg`**, mirroring the float
    twin. Self-contained, ~14 sites, byte-identical while the register
    chosen stays RAX. THIS IS THE GATE - nothing else can proceed.
 2. Convert the emitters family by family to `alloc_scratch()`, with
    `vdjcmp` proving byte-identical at every step and the census as the
    progress bar.
 3. Only when a register's count reaches ZERO, admit it and re-run the
    MAXPINS sweep. One register at a time, never by analogy.

**Honest scale: ~740 mechanical edits across essentially every emitter
in a 16k-line file, for a projected -3.8% (83_regs_int_40) to -5.6%
(80_regs_int_08) once the first of the three lands.** That is a
multi-session conversion, and it buys NOTHING until a register's count
hits zero - so it should be started only with that commitment, and step
1 should be done first regardless since it is small and unblocks
everything.


## 2026-08-18 (k): step 1 DONE - lever A's protocol no longer names RAX

`JitFwd::in_rax` -> `in_temp` + `in_reg`, plus `res_reg` on the
producer side, mirroring the float twin that was generalised for the
same reason (C4b inc 2, "no longer always xmm0"). `emit_fwd_bump` - the
one function every consumer already calls exactly once, immediately
before using the value - became the ADAPTER: it emits `mov rax, in_reg`
when they differ, so all the consumer code below keeps its "it is in
RAX" assumption while the PROTOCOL stops requiring it.

Byte-identical (vdjcmp 108/108), because every producer still picks RAX.

**PROVEN BY CONSTRUCTION, not by inspection.** A probe producer that
hands its result over in RCX *and destroys RAX*:

    adapter PRESENT   1923/1923
    adapter REMOVED   1898/1923

⛔ The FIRST version of that probe was VACUOUS and passed both ways: it
COPIED to RCX and left RAX intact, so the adapter was never needed.
A hand-over probe must kill the old register.

### ⛔ AND IT FOUND A REAL LATENT DEFECT IN INCREMENTS 2 AND 3

The TESTS-only counter bumps added in those increments use **RCX** as
scratch and were emitted **before** `emit_fwd_bump` - so they would
have destroyed exactly the register a future producer hands its value
over in. Invisible today (in_reg is always RAX), fatal the moment it is
not, and precisely the r9 shape: a latent clobber nothing exercises.
The adapter now runs FIRST in all four converted paths, and the
ordering is commented as reasoned rather than tried, since no test can
currently observe it.

### Next

Step 2: convert emitters family by family to `alloc_scratch()`, vdjcmp
byte-identical at each step, `scripts/regcensus.py` as the progress bar
(RAX 399, RCX 222, RDX 118). Step 3: at a count of ZERO, admit that
register and re-run the MAXPINS sweep. One at a time, never by analogy.


## 2026-08-18 (l): step 2 increment 1 - the RDI arithmetic family, and
## THE COMPLETE RDI ADMISSION AUDIT (done; the admission itself is next)

The compound element store's arithmetic tail lost its hardcoded RDI:
`add_rax_rdi` / `sub_rax_rdi` / `imul_rax_rdi` / `idiv_rdi` are DELETED
in favour of `op_rr2(aop, dst, src)` (increment 2's seam) and a new
`idiv_reg(divisor)`. All three call sites already had the ElemScratch
plan threaded for obj/data/count/idx - only `val` was hardcoded - so
each three-line if/else collapsed to one plan-driven line.

Census: **RDI 27 -> 20, RAX 402 -> 393.** Two registers from six edits,
because a fixed-pair wrapper names BOTH.

### THE PLAN'S ORDERING WAS WRONG - convert by REGISTER VALUE, not by
### alloc_scratch's preference order

Entry (j) said convert RAX first because `alloc_scratch` prefers it.
That optimises the wrong thing. The payoff is per-REGISTER (a pin is
worth -0.63%/pin on 83_regs_int_40, -2.8% and rising on 80_regs_int_08),
so the right order is CHEAPEST-REGISTER-FIRST:

    RDI  20 sites   <- pin 8
    RSI  25 sites   <- pin 9   (11 of the 25 are the t_int singleton,
                                live ONLY in the non-default no-arena
                                build, and already behind the
                                store_type_tag / cmp_reg_tag seam)
    R8   48, R10 63, R11 33    <- already in the pool
    RDX 118, R9 103, RCX 222, RAX 393

RDI+RSI is ~45 sites for pins 8 and 9 - projected to capture most of
the -3.8%/-5.6% the whole 740-site conversion was costed at.

### ⛔ THE RDI AUDIT - ITS OWN SITES AGAINST THE GATES, NOT BY ANALOGY

r9 shipped a wrong answer for a day because it was admitted "safe by
the same gates as r10/r11". So RDI's 20 sites are enumerated here and
each is accounted for individually. THREE are not blockers at all:

  - the `enum Reg` continuation line (a census artifact);
  - `ElemScratch::val = RDI` (a PREFERENCE, and `pick` tests
    `elem_reg_usable` - which tests `reg_holds_pin` - before honouring
    it);
  - `CAND[] = { RDI, ... }` (same guard).

The remaining 17 sit in exactly FOUR functions:

  fn                      sites  why a pin in RDI survives
  ----------------------  -----  ---------------------------------------
  emit_sync_push_native     3    jit_run_blocks_xcache denies the WHOLE
                                 caller-saved pool to any run holding
                                 CallV/CachedCallV/CallValueV - which are
                                 precisely the ops that emit this
  emit_sync_call_inline     3    same gate, AND a prologue precedes them
                                 inside the function
  emit_ret_native           5    its SECOND LINE is `e.flush_cache()`,
                                 so no pin is live past it
  emit_op                   1    prologue precedes it in-function
  frag_entry                3    RDI is the fragment's INCOMING window
                                 argument, consumed into rbx by the
                                 entry's last instruction; pins load
                                 AFTER frag_entry, never before
  accessor bodies           2    lea_rdi / slots_to_arg0 definitions

Each gate was READ, not assumed: jit_run_blocks_xcache's switch lists
the three call ops; emit_ret_native line 2 is the flush.

**CONCLUSION: RDI is admissible.** The remaining work to admit it is to
add 7 to XCACHE_ORDER and run the matrix - the sites need no further
conversion, because every one is already covered by an existing gate.
NOTE frag_entry is the one that constrains ORDER rather than being
spilled: a pin load must never precede `mov rbx, rdi`.

### ⛔ AND THE CENSUS WAS UNDER-REPORTING THE REGISTER IT WAS CLEARING

RDI read **15** while the truth was **20**. The five it could not see:

    static constexpr uint8_t REG_ARG0 = 7;   /* rdi */
    push_reg(REG_ARG0);   pop_reg(REG_ARG0);
    mov_rr(REG_SLOTS_BASE, REG_ARG0);

The register behind a named CONSTANT - the sixth audit-table shape one
level deeper than correction 3, which caught the register in a METHOD
NAME. This is the r9 failure exactly: acting on a count from a tool
blind to its own subject, for a register about to enter the pin pool.
FIXED by DERIVING the alias map from the declarations
(`static constexpr uint8_t REG_\w+ = N;`), so a new alias counts the day
it is written - not by a hand-written list, which is the stale-table
trap one level up.

### Next

Admit RDI (XCACHE_ORDER gains 7; MAX_XCACHED 3 -> 4), run -rt +
corpus_diff plain/--levers/--xrot/--nolowmem - **--xrot now sweeps FOUR
rotations, and it is the net that would have caught r9 in seconds** -
then re-run the MAXPINS sweep to price pin 8 against the projection.
Then RSI for pin 9.


## 2026-08-18 (m): RDI ADMITTED - pin 8 - and the starvation the
## nolowmem lane caught the same day

`XCACHE_ORDER` is `{ 10, 11, 8, 7 }`; `jit_pin_budget()` is **8**
(MAX_CACHED 4 + MAX_XCACHED 4). The audit in (l) was sufficient: no
site needed converting, because every one was already covered by an
existing gate. Blast radius: 7 of 109 corpus programs changed emitted
code, 0 failed.

### ⛔ AND IT STARVED THE ELEMENT TIER OFF-ARENA

`elem_scratch_plan` allocates two ISA-unconstrained roles - `idx` and
`val` - from `CAND = { rdi, r9, r10, r11, rsi, r8 }`, and DECLINES the
whole tier to the helper if it cannot fill both.

  arena ON  : tags are imm32, so rsi and r8 are ordinary candidates.
  arena OFF : rsi carries t_int and r8 t_float, so elem_reg_usable
              denies BOTH; a C1 hoist takes r10/r11; pinning rdi then
              leaves ONE candidate for TWO roles -> !ok -> every
              hoisted compound element store falls back to the helper.

Measured: `-rt` **1921/1923 off-arena in BOTH build types** ("the
hoisted-compound arm never ran", plus a stale expectation in
`jit_xcache_pins` at the two new rotations), **1923/1923 on-arena in
both**. The debug+arena lane - the one run by reflex - was green
throughout.

**This is the nolowmem lane paying for itself a second time.** It was
built (2026-08-18) because a configuration only some platforms can take
is a configuration nobody tests; its first run found a shipped wrong
answer in `store_dst_bool`. Its second found this, on the same day the
register it concerns was admitted. A register-allocation change that
looks local is not: it competes with every OTHER consumer of the
register file, and the tightest consumer is the one in the
configuration with the fewest registers.

FIXED in the established pattern - each contributor names its own
registers in `jit_xcache_clobber`: rdi is spendable as a pin only when
`jit_tag_is_imm(t_int)`, i.e. only when the arena freed rsi/r8 for the
element tier. Costs nothing in the shipping configuration.

KNOWN COARSENESS, deliberate: the pin is picked once per RUN, before
any element op is emitted, so this denies rdi off-arena for every run
rather than only for runs containing element ops. `jit_xcache_busy`
already scans a run for tag use; extending that scan to element ops
would recover rdi for off-arena runs that never touch an array. Worth
doing only if the off-arena configuration is ever measured.

### Next

RSI for pin 9 - but NOTE the above changes its calculus: rsi is a
candidate the element tier RELIES on off-arena, so admitting it needs
the same self-naming treatment, and the finer-grained
`jit_xcache_busy`-style element scan may become the prerequisite rather
than an optional follow-up. Re-run the MAXPINS sweep first to price
pin 8 against the -0.63%/-2.8% projection - that measurement is the
gate on whether pins 9+ are worth their conversion cost at all.


## 2026-08-18 (n): MAINTAINER DIRECTIVE - 13 REGISTERS IS NOT OPTIONAL

**Stop pricing each register's marginal benefit.** The maintainer's
position, 2026-08-18: getting to 13 usable registers is a REQUIREMENT,
not a trade. The bench corpus is small; larger scripts accumulate the
benefit, and *a register allocator that cannot use all the registers is
not worth its complexity*. So the sweep numbers are a CHECK that each
admission works, never a gate on whether to do it.

Consequence for this file: entries (l)-(m) argue cost-vs-benefit
ordering. That reasoning is superseded - cheapest-first remains the
sensible ORDER, but every register gets done regardless of what its
pin measures.

## RSI - pin 9: THE AUDIT (complete; execution is the next step)

25 census sites, by enclosing function:

  emit_sync_push_native      6  ) the SAME four gates already verified
  emit_sync_call_inline      3  ) for rdi in (l): jit_run_blocks_xcache
  emit_ret_native            1  ) denies the pool to runs with a call,
  emit_op                    1  ) flush_cache() is emit_ret_native's
                                ) second line, prologue for emit_op
  emit_store_elem2_inline    5  ) the t_int SINGLETON fallback, reached
  emit_store_elem            1  ) through store_type_tag / cmp_reg_tag /
  store_dst                  2  ) cmp_rax_tag - all of which take the
  emit_float_load            1  ) register as an ARGUMENT (#96 step 3)
  emit_call_epilogue         2  ) the singleton MATERIALISATION, already
  emit_type_tags             1  ) guarded by !reg_holds_pin(RSI)
  elem_reg_usable            1  ) the guard itself and CAND - not
  elem_scratch_plan          1  ) blockers (both test reg_holds_pin)

**So rsi needs NO site conversion either.** Its 11 raw-scratch sites are
in the four functions already proven safe for rdi; the other 12 are the
t_int singleton, which is live ONLY off-arena.

### THE CHANGE

1. `jit_xcache_busy` gains the INT-tag contributor mirroring r8's float
   one:

       if (run_needs_int_tag(...) && !jit_tag_is_imm(t_int))
           busy |= 1u << 6;            /* rsi carries t_int */

   NOTE r8's `run_needs_float_tag` gate does DOUBLE DUTY (see the
   comment there): it also excludes r8's non-tag scratch paths. Check
   whether an int-tag equivalent needs the same breadth, or whether the
   four-function audit above already covers it - do NOT assume.
2. `XCACHE_ORDER` gains 6.

### ⛔ AND THE ELEMENT-TIER RESERVATION IS NOW A PREREQUISITE, NOT A
### FOLLOW-UP

`elem_scratch_plan` needs TWO free members of
CAND = { rdi, r9, r10, r11, rsi, r8 } for `idx` and `val`, and declines
the whole tier to the helper otherwise. Every register admitted to the
pin pool shrinks that set - this already bit once (entry (m): rdi
starved it off-arena, -rt 1921/1923).

With rsi pinned ON-arena and a C1 hoist holding r10/r11, CAND survivors
are { r9, r8 } - exactly two, the minimum. **The next register admitted
(r9 is in CAND) breaks it**, and (m)'s per-register workaround does not
generalise.

So before or with rsi, replace the ad-hoc "rdi names itself off-arena"
rule with a GENERAL reservation: at pool-pick time, deny pool members
until at least two CAND members remain usable by `elem_reg_usable`, for
any run containing an element-tier op (scan it the way
`jit_xcache_busy` already scans for tag use). That is the rule that
survives r9, rdx, rcx and rax joining.

Watch it fail: the `hoist` -rt cases ("the hoisted-compound arm never
ran") are the detector, and they only fire off-arena today - construct
an ON-arena case too, or the reservation goes untested in the shipping
configuration.

---

## (o) 2026-08-18 - THE GENERAL RESERVATION, AND RSI ADMITTED (pin 9)

Both halves of (n), built in that order. `-rt` green in both arena
configurations and both build types, corpus_diff green on all five
matrices, and the emitted code byte-identical on-arena for the
reservation alone (109/109) before rsi changed it.

### THE RESERVATION - `elem_scratch_reserve` (jit.cpp)

The ad-hoc rule it replaces was one line:

    if (!jit_tag_is_imm(jit_layout().t_int))
        clob |= 1u << 7;                 /* rdi: keep it for CAND */

correct, and useless past one register: it names rdi, and #96 puts five
more into this pool. rsi is the next one and is ITSELF a candidate, so
the ad-hoc form would grow a clause per admission, each re-deriving the
arithmetic - a family of `&&`s becoming an audit table nobody re-reads
(CLAUDE.md's fifth shape).

The general rule COUNTS instead. At pool-pick time:

  - take the candidates this run can use for reasons other than a pin -
    `elem_reg_usable_nopin(r, float_tag_live, hoist_claimed)`, which is
    the SAME predicate the emitter asks, split out of
    `elem_reg_usable` so there is one rule set and not two;
  - separate the ones no pin can reach (outside the pool, or already
    clobbered) from the ones this pool could spend;
  - withhold spendable members, LEAST-PREFERRED FIRST, until
    `ELEM_ALLOC_ROLES` (2) are guaranteed to survive.

Three properties worth keeping:

  - **it is conditioned on the RUN** (`run_has_elem_scratch`), so a run
    with no element store pays nothing. That is strictly less coarse
    than the rule it replaces, and it showed immediately: the
    `jit_xcache_pins` hoist case expected a DECLINE off-arena and now
    engages, because its element ops are READS, whose emitter takes no
    ElemScratch. The expectation was stale in the good direction and is
    now `true` in both configurations - a stronger assertion;
  - **it walks the UNROTATED XCACHE_ORDER backwards.** `take_reg` scans
    forwards, so the tail is handed out only at maximum pin count and
    is the cheapest member to give back. Deliberately not rotated:
    MYLANG_JIT_XROT must not change emitted code, only which member is
    preferred;
  - **the op list SELF-CHECKS.** `elem_scratch_plan` now takes the
    opcode and ML_CHECKs it against `op_uses_elem_scratch`, so a third
    emitter that takes a plan without registering its op aborts BY NAME
    instead of silently losing its inline tier. A stale list here would
    defeat the reservation's own purpose.

### THE INSTRUMENT: `elem_noreg` / `elem_reserve` (MYLANG_JITSTATS)

Two emit-time counters, because the decline they describe is otherwise
invisible - the helper computes the same answer, so only the speed
changes. Corpus-wide (bench + samples + functional):

    on-arena, before rsi   elem_noreg 0   elem_reserve 0
    off-arena, before rsi  elem_noreg 0   elem_reserve 17
    on-arena, after rsi    elem_noreg 0   elem_reserve 14

i.e. the reservation absorbs exactly the pressure each admission
creates, and nothing has ever starved with it in place.

### RSI (6) ADMITTED - pin 9, and TWO SITES THAT HAD ROTTED

The audit in (n) held: 24 census sites, 12 the t_int singleton (live
only off-arena, all reached through the register-taking seams), 10 raw
scratch in the four functions rdi's audit already cleared, 2 non-uses.
`jit_xcache_busy` claims rsi outright off-arena - deliberately
UNCONDITIONAL where r8's is gated, because `emit_type_tags` has no
`int_tag_live` analogue: off-arena every fragment entry materialises
t_int into rsi.

But the audit's tidy list missed two sites that had quietly stopped
being true, and BOTH had to be fixed before rsi could join:

 1. **`emit_store_elem` staged its helper arguments BEFORE
    `emit_call_prologue`** - the only call site in the file to invert
    the order (compare `emit_dict_store` directly below it). The
    comment said "read before the cache regs spill"; the prologue's own
    comment says a spill does not INVALIDATE, so the premise was false.
    With an argument register pinnable it is a WRONG ANSWER: the stage
    overwrites the pin and the prologue then spills the overwritten
    register to the pin's slot. Fixed by emitting the prologue first.
 2. **`emit_op`'s boxed int-arith arm did `movabs rsi, t_int`
    unconditionally** to feed `store_dst`, whose tag write became an
    imm32 in #96 step 3. Dead code on-arena - and a pin clobber. This
    is the exact MIRROR of the `store_dst_bool` bug: there an argument
    outlived its loader, here a loader outlived its reader. Both are
    found by asking `jit_tag_is_imm` at the site, and both are the same
    lesson - **an optimization that changes whether a register is
    involved must fix BOTH ends.**

Measured on the corpus: `movabs rsi, <int-tag>` occurrences 3 -> 0 over
the sampled benches, and rsi now appears as a pin (83_regs_int_40: two
rsi moves -> four).

### THE DETECTOR, WATCHED FAILING

`jit_elem_scratch_reserved` (tests.cpp). Eight hot int accumulators, a
runtime bound, and an element store on a loop-invariant base so a C1
hoist claims r10/r11 - which is what makes the candidate list tight.

    with the reservation      store_fast 80  elem_noreg 0  elem_reserve 1
    with it returning 0       store_fast  0  elem_noreg 2  elem_reserve 0

⛔ **`-rt` WAS GREEN AT 1923/1923 UNDER THAT SABOTAGE** before this
check existed, and so were all four differentials and corpus_diff. The
tier vanished completely and no net in the project could see it. That
is why the case asserts a COUNTER and not a value - the same shape as
#96 step 3's nested-store tier, which emitted perfectly and reached 0
of 64.

It carries an ANTI-VACUITY assertion (`elem_reserve > 0`): the day the
pool shrinks or ELEM_CAND grows, the case says so instead of passing
having tested nothing.

### WHAT THIS CHANGES FOR THE REMAINING FOUR

r9, rdx, rcx, rax are all in the ISA-fixed or candidate sets, so the
reservation is what makes each of them admissible at all. It is now
generic: nothing about it names a register, and adding one to
XCACHE_ORDER is answered by the count.

---

## (p) 2026-08-18 - r9: the READ-tier seam, and what is left

The element READ tiers now take their index register as an ARGUMENT
(`load_slot_idx`, `load_index_idx`, `emit_elem_bounds_or_wrap`,
`emit_flat_int_tail`, plus a `test_rr` encoder for the one hand-emitted
`test r9, r9`). Landed INERT - byte-identical over 109 programs in both
arena configurations.

⛔ **THE CENSUS WENT UP, 103 -> 117, AND THAT IS CORRECT.** A helper
whose NAME held the operand was hiding ~30 call sites from the count
that decides admission. Expect the same for rdx/rcx/rax and do not read
it as a regression - it is the tool finally seeing its subject, exactly
as when the accessor list became derived.

### THE REMAINING r9 WORK, grouped

CONVERT - the read tiers, same threading as the seam above:

     28  emit_op            (its element arms + `movabs R9, t_arr`)
     25  emit_load_elem2_inline
      6  emit_elem_int_read
      5  emit_elem_base_gate

ALREADY SAFE - by the gates rdi and rsi were cleared against:

     10  emit_sync_push_native / emit_sync_call_inline
             (jit_run_blocks_xcache denies the whole pool)
      5  emit_ret_native   (flush_cache() is its second line)

AUDIT - not yet classified:

      7  emit_branch, jit_compile_chunk, jit_type_singletons,
         emit_nstack_switch_post

⛔ THE HOLE - no existing gate covers it:

     ~6  emit_ctx_chain_r9, emit_ref_check_jae_r9,
         load_r9b / store_r9b

The last row is the one that matters. It is the CAPTURE / global-slot
chain - the very site that printed 88854283473440 - and **no existing
gate covers it**: `LoadCaptureV` is not a call op, so
`jit_run_blocks_xcache` says nothing about it. Two options:

 1. convert it too (`[r9 + disp]` becomes `[reg + disp]`, so `load_r9b`
    / `store_r9b` take a base register); or
 2. add a per-register run predicate - "a run containing a capture op
    may not spend r9" - fed into `jit_xcache_clobber` exactly as the
    tag singletons and the hoist pair are. Captures are rare, so this
    costs almost nothing and is the smaller step.

(2) first, then (1) if a closure-heavy shape ever wants the register.
Either way r9 also stays in ELEM_CAND, so `elem_scratch_reserve` will
withhold it whenever the element tier needs it - which is now automatic
and needs no new clause.

### THEN rdx, rcx, rax

`ElemScratch` already records why `obj` (rax) and `count` (rdx) are
ISA-FIXED: the compound `/=` and `%=` arms emit `cqo; idiv <val>`,
which reads the dividend in RDX:RAX. Those two are not freed by
threading - they need the idiv arm to spill/restore around itself, or
the arm to decline when either register is pinned. rcx is the SIB base
of every element operand and the shift-count register. Expect these
three to be the real work of #96, with rax (393 sites) last.

---

## (q) 2026-08-18 - r9 REJOINED (pin 10), and the latent bug it uncovered

`mylang -v`: **`jit_pins 10 ... xcache 6 caller-saved`**. -rt green in
{debug ASan/UBSan, release TESTS+VM_HARDENING} x {arena, no-arena};
corpus_diff green on all five matrices (--xrot now sweeps SIX
rotations); the historical wrong-answer repro prints 56640 at every
rotation.

r9 is the register that shipped a wrong answer for a day. It is back
only because every site the first attempt missed is now an ARGUMENT
instead of a name. Census: **103 -> 23**.

    the element READ tiers   -> elem_read_idx(e, op), threaded through
                                every emitter and every arm
    the element STORE tiers  -> ElemScratch's sc.idx, which they had
                                ALREADY allocated and then ignored
    the CAPTURE/global chain -> ctx_chain_reg(e) + load_base/store_base
    the C1 hoist nav         -> its t_arr compare uses RCX

The 23 that remain are the `enum Reg` line, the ElemScratch default,
ELEM_CAND and the two allocators' preference (definitions), plus 16 in
emit_sync_push_native / emit_sync_call_inline / emit_ret_native /
emit_nstack_switch_post - the group `jit_run_blocks_xcache` and
`emit_ret_native`'s `flush_cache()` already cover.

### ⛔ THE LATENT BUG: A PLAN THAT WAS NAMED AND IGNORED

Eleven sites in the store tiers read

    load_index_r9(e, in);          // loads the index into r9
    e.cmp_rr(sc.idx, sc.count);    // ...but compares sc.idx

The ScratchPlan's entire promise is "the emitter is TOLD which register
holds each role". This role was told and ignored. It would have made
this admission wrong AGAIN - the plan picks a non-r9 index precisely
when r9 is pinned, which is the case r9 joining the pool creates.

It was invisible for one reason: `load_index_r9` put the register in
the NAME, so nothing in the source visibly disagreed with `sc.idx`.
Turning the register into an argument is what made the disagreement
legible. **That is the argument for the seam in one line**, and it is
the sixth audit-table shape paying for itself.

### THE RESERVATION CARRIED IT

Nothing new was needed: `elem_scratch_reserve` already withholds pool
members until the run's neediest element op has its candidates, and r9
leaving the guaranteed-survivor set is just a smaller starting count.
Corpus-wide on-arena: **elem_noreg 0, elem_reserve 55** (was 0/14 at
nine pins). The withholding order - reverse XCACHE_ORDER, so the
newest and least-preferred member goes first - hands r9 back to the
element tier before anything else, which is exactly where it is most
useful.

### REMAINING: rdx, rcx, rax

    RDX  118 unbracketed    ISA-FIXED as ElemScratch::count, and idiv's
                            RDX:RAX dividend
    RCX  222                the SIB base of every element operand, and
                            the shift-count register
    RAX  393                ElemScratch::obj, idiv's quotient, every
                            helper's return value

These are not freed by threading. Each needs the idiv arm and the
element navigation to either spill around themselves or DECLINE when
the register is pinned - a different mechanism from the one that got
rdi, rsi and r9 in. Do rdx first (it is the smallest and its only hard
constraint is idiv), then rcx, then rax.

### (q1) the rdx survey, for whoever picks it up

118 unbracketed sites, by enclosing function:

    ALREADY SAFE (38)   emit_sync_push_native 21, emit_ret_native 16,
                        emit_sync_call_inline 1 - the same gates
                        rdi/rsi/r9 were cleared against
    THE COUNT ROLE (39) emit_load_elem2_inline 15,
                        emit_store_elem_inline 14,
                        emit_store_elem2_inline 6,
                        emit_elem_bounds_or_wrap 4,
                        emit_flat_int_tail 3, emit_elem_base_gate 1
                        - already a NAMED role (ElemScratch::count),
                          just fixed to rdx. Threading is mechanical.
    emit_op (29)        helper args, the div/mod arms, boxed ops
    emit_div_magic (3)  RDX:RAX is the ISA, not a habit
    the TESTS bumps     now all through bump_counter, so they take the
                        register as an argument

**The hard core is small**: `idiv` puts the dividend in RDX:RAX and
writes the remainder to RDX, and `emit_div_magic` needs the same pair.
Everything else is the `count` role, which the ScratchPlan already
names. So the rdx step is: make `ElemScratch::count` allocatable,
and have the compound `/=` / `%=` arm either spill rdx around itself
or DECLINE when rdx is pinned (it already declines for a literal 0/-1
divisor, so the decline path exists and is tested).

⛔ **Check the ScratchPlan is HONOURED, not just consulted.** The r9
conversion found eleven store-tier sites that loaded into r9 while
comparing `sc.idx` - the plan named a register and the code ignored it,
invisible because the register was in the loader's NAME. `count` is the
same kind of role; audit every `RDX` inside the element tiers against
`sc.count` before assuming they agree.
