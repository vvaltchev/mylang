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

---

## (r) 2026-08-19 - RDX (pin 11), by a POSITIVE run predicate

`jit_pins 11 ... xcache 7 caller-saved`. The first register admitted
WITHOUT a site audit, because rdx cannot have one: ~100 unbracketed
sites, several ISA-forced (`idiv`'s RDX:RAX, emit_div_magic).

`run_may_pin_rdx` inverts the question the way `run_needs_float_tag`
does - rdx is spendable only in a run made ENTIRELY of ops positively
established never to write it, so an unlisted or new opcode costs a
pin and never an answer.

### ⛔ TWO THINGS TO CARRY INTO rcx AND rax

**1. A POSITIVE PREDICATE CAN BE VACUOUSLY EMPTY, AND -rt WILL NOT SAY
SO.** The first version omitted `ReturnV`. A leaf body is ONE run
ending in ReturnV, so the predicate refused EVERY fragment in the
corpus: `-rt` green, corpus_diff green, zero registers gained. Only
counting the emitted pin loads found it. When you admit rcx or rax the
same way, MEASURE REACH BEFORE BELIEVING THE ADMISSION -
`MYLANG_RDXDBG=1` (add a sibling) names the op that refused.

**2. WIDENING THE POOL EATS TEST SHAPES, AND THE DANGEROUS ONES FAIL
SILENTLY.** Three `jit_two_address` cases were built on "TEN
accumulators outnumber the pool". At eleven pins one failed loudly and
**two passed while testing nothing** - including a DECLINE case
asserting `hits == 0` about a tier that could no longer fire.

Before admitting the next register, grep the tests for hardcoded
accumulator counts and any comment of the form "N, so they outnumber
the pool". The fix is always the same: derive N from
`jit_pin_budget()`, and take the expected VALUE from the tree-walker,
because a derived N changes the answer.

### THE REACH, AND WHAT WOULD WIDEN IT

rdx is spent at 107 pin entry loads across 36 programs, but only
in runs with no element
and no div/mod op - dense scalar int loops. Widening means threading
the element tiers' `count` role exactly as `idx` was threaded for r9;
the eleven fixed-pair accessors deleted in the previous commit are the
prerequisite, and every one of those sites now takes its register as an
argument.

### NEXT: rcx (218), rax (390)

    rcx   the SIB base of every element operand, the shift COUNT
          register (`sar`/`shl` by cl), and ElemScratch::data
    rax   ElemScratch::obj, idiv's quotient, every helper's return
          value, and the boxed tiers' accumulator

MEASURED, so this is not a guess: of the four shared paths every
whitelisted op reaches,

    store_dst      RAX 3   RCX 0
    write_slot     RAX 0   RCX 0
    load_operand   RAX 0   RCX 0
    op_rr          RAX 0   RCX 0

**rax is a dead end for the predicate route.** `store_dst` is on the
tail of essentially every producing op, so an rax whitelist is close to
empty and a run predicate would buy nothing. rax needs the roles
threaded - note `store_dst` ALREADY takes its source register as an
argument, so the question is only its three internal uses.

**rcx: the predicate route is DEAD TOO, and here is the exact reason.**
None of the four shared paths touch it, but the specialized-arith arm
has THREE emission paths and only two are rcx-free:

    two_addr (memory dst)   mov rax, b ; <op> [dst], rax    rax only
    two_addr_reg (pinned)   op_rr(dst_reg, src_reg)         neither
    the general staging     mov rax,a ; mov rcx,b ; op ; st rax AND rcx

Which path an op takes is decided at EMIT time, from five conditions
(dst == operand a, dst not pinned / pinned, imul, forwarding, ref-list).
A run-level predicate must assume the worst, so ANY specialized op in
the run denies rcx - and those are exactly the ops that make rdx
spendable. The whitelist is therefore empty in practice: no run
qualifies. Confirmed by construction, not by trying it.

So rcx needs its ROLES threaded, like r9. Its 218 sites, grouped:

    26  emit_sync_push_native  ) ALREADY SAFE - the gates rdi/rsi/r9
    13  emit_ret_native        ) were cleared against
     4  emit_sync_call_inline  )
    22  emit_load_elem2_inline ) ElemScratch::data, the SIB BASE of
    12  LoadElemInt/Float arm  ) every element operand. The encoders
    10  emit_store_elem_inline ) (load_elem_q/zx8/sd, store_elem_*)
     8  emit_store_elem2_inline) ALREADY take the base as an argument,
     5  LoadElemBool arm       ) so this is threading, not re-encoding
     4  emit_flat_int_tail     )
    15  IntAddStep/ForLoopStep ) the loop-step staging
     7  IntOrRR.. (the family) ) the general three-address staging -
                               ) allocate it the way elem_read_idx does
    12  LoadCaptureV/StoreCaptureV ) the chain's copy register
     ~40 the long tail (UnaryV 7, LoadMemberFloat 5, StructCtorV 4,
         MoveV/LoadBuiltinV/LoadConstV 3 each, jit_try_container 8, ...)

ORDER: the element `data` role first (55 sites, one mechanical pass,
and it doubles as the rdx `count` work since the two are computed
together), then the arith staging, then the tail. Expect it to be
larger than r9's conversion and to land inert at every step -
`scripts/vdjcmp.sh` byte-identical is the check that it did.

## (s) the rcx element pass - and THREE shipped bugs it turned up

Done, 2026-08-19, over four commits. Census across the day:

               start   after (s)
    RCX          218      159
    RDX          118       62
    RAX          393      340

Threaded: the `LoadElemInt/Float` arm, the `LoadElemBool` arm, the
nested READ (`emit_load_elem2_inline`, all 22 RCX + 13 RDX), and BOTH
element STORE tiers. Two fixed-pair accessors deleted
(`mov_rax_rcx_d`, `cmp_byte_rcx`) for a generic `cmp_byte_base`; four
generic encoders added (`lea_base`, `lea_elem_q`, `cmp_reg_base`,
`load_base0`) over a shared `emit_modrm_disp`.

**Every step verified byte-identical over the corpus in BOTH arena
configurations**, which is the property that makes a conversion this
size safe to do quickly. Two caveats learned the hard way:

 - **byte-identity is only evidence where the arm is EMITTED.** The
   corpus emits no `LoadElem2Float` at all, so the float and row-slice
   arms had to be covered by purpose-written probes (now
   `tests/functional/16_elem2_fused.my`);
 - **HALF-threaded is worse than un-threaded.** `emit_store_elem2_inline`
   already READ `sc.data`/`sc.count` in its bounds compares while
   WRITING RCX/RDX in the loads above them - correct only by the
   defaults, and it reads as done.

### The three bugs, all pre-existing, all found by one new corpus file

1. **A slice read the wrong element under the float op.** The float
   branch of the nested read `return`ed before the two SLICE arms were
   emitted, leaving their forward `jne rel32`s unpatched - so they fell
   through into the non-slice path and dropped the slice's `off`. An
   `if`/`return` where an `if`/`else` was meant.
2. **A rel8 span that grows with the pin budget.** `emit_ref_check`'s
   short `jb` jumps over a helper call, whose length is the number of
   live caller-saved pins. Clean at MAXPINS 4-6, aborts from 8 up, and
   at `ASSERTS=0` truncates the displacement instead - a jump into the
   middle of an instruction. **The rule was already written in
   `patch8`'s comment.** Now machine-checked: `Emitter::n_prologues`.
3. **The divmod cold gate SIGFPEs.** Five hand-encoded byte sequences
   naming `rdi` (the divisor) and `r9` (the index) literally, while
   both are ALLOCATED roles the allocator moves routinely. `/= 0`
   reached the native `idiv`.

**All three are the same failure**: a fact stated in a form no scan
can read - a register in a byte literal, a register in a method name,
a call behind a helper's name, a rule in a comment. And all three
survived because the nets are blind to them by construction: with each
defect reintroduced, `-rt` is GREEN at 1924/1924 every time.

### What is left for rcx (159), and the order

     ~55  emit_op's tail - the arith staging, UnaryV, StructCtorV,
          LoadMemberFloat, MoveV/LoadConstV/LoadBuiltinV
      21  emit_branch - IntAddStep / ForLoopStep staging
      12  LoadCaptureV / StoreCaptureV - the chain's copy register
       8  jit_try_container
      39  emit_sync_push_native + emit_ret_native - ALREADY SAFE by
          the existing gates; convert last, for the census only

Then make `data`/`count` ALLOCATABLE in `ElemRead` and `ElemScratch`.
That is the step with real risk: it raises the reservation's role count
from 2 to 4 for a store, and with a C1 hoist holding r10/r11 out of six
CAND members a four-role store may start DECLINING. `g_jit_elem_noreg`
exists to say so rather than let it go quiet - watch it, and widen
`ELEM_CAND` before accepting a decline rate.

Then rax (340), which has no predicate shortcut: `store_dst` touches it
3x on every whitelisted path, so the roles must be threaded first.

## (t) the branch emitter, the tag seam, and what the census cannot see

Four more commits, 2026-08-19. Census: RCX 159 -> 140.

 - **`op_rr(e, aop)` took its operands** - a fixed rax/rcx pair across
   seven call sites, none naming a register. Kept its own encoding
   direction rather than folding into `op_rr2`, so it stays inert.
 - **`emit_branch` gained the STAGING role** (21 sites) plus three
   write helpers (`tmp_lit`/`tmp_slot`/`tmp_operand`) that declare the
   clobber at the write. **Three stale `e.scratch(RCX)` deleted** -
   they sat next to a `bump_counter` that clobbers nothing, so they
   asserted a register the path does not write, and an over-broad
   `scratch()` ABORTS a legal fragment once the register is pinnable.
 - **The tag seam's remaining holes filled**, which turned up bug 4
   (the boxed truth test comparing against an rsi nobody loads on the
   arena). `cmp_rax_rcx()` deleted - the FIFTH fixed-pair accessor,
   missed by the 2026-08-17 sweep. Measured -1.19% Ir on the two `dyn`
   benches, -20% on a pure boxed-truth-test loop, nothing regressed.
 - **The ref-check family and `emit_ctx_chain` stopped hand-encoding**;
   the four gate helpers take (and now DECLARE) their scratch.

### ⛔ THE CENSUS WAS CONFIDENTLY WRONG, AND NOW SAYS SO

`regcensus.py`'s three recorded corrections all assume the register
appears as a NAME - an operand, a method name, an alias constant. **A
register spelled into a modrm or SIB byte is matched by none of them**,
and there are **290 such lines** in jit.cpp. One of them was bug 3's
SIGFPE.

Decoding modrm there would make the script a disassembler, and a
half-right one is worse than none, so it does the other thing an
instrument may do: it counts them and prints an explicit BLIND SPOT
banner, with the table labelled a LOWER BOUND (`--raw` lists them).

**That count is now the real progress metric for the rest of this
arc** - more honest than the per-register columns, because converting
a hand-encoded instruction to a generic encoder moves it from
"invisible" to "counted", which makes the register columns go UP while
the code gets safer. Both numbers have to be read together:

    hand-encoded lines   290   <- drive this to 0
    RCX / RAX columns    140 / 357   <- these RISE as it falls

### Still to do for rcx (140)

     ~55  emit_op's tail (arith staging, UnaryV, StructCtorV,
          LoadMemberFloat, MoveV/LoadConstV/LoadBuiltinV)
       8  jit_try_container
      39  emit_sync_push_native + emit_ret_native - safe by the
          existing gates; convert last, for the census only
     ~38  the scattered remainder

## (u) the disassembler, and emit_op's roles

**THE DISASSEMBLER FIRST** (maintainer: "I wanna see NO instruction
whatsoever that cannot be decoded correctly - that's a hard
requirement"). It was not met, and the reason it looked met is the
whole lesson: `DUMP IS UNRELIABLE` counts `.byte` lines, so it can only
report bytes we KNOW we failed on. A decoder cannot check itself.

 - **`scripts/disasmcheck.py`** - `MYLANG_VDJ_HEX=1` puts each
   instruction's raw bytes in the dump; the script hands each fragment
   to **objdump** and compares BOUNDARIES and MNEMONICS. Now in the
   Nets CI lane with `--matrix` (both arenas x 7 pin rotations x 5 pin
   budgets - the axes that change WHICH encodings get emitted).
   **2,209,682 instructions, 0 disagreements.**
 - It found `F7 /3` (neg) and `F7 /5` (imul) missing on **284 corpus
   sites**, printing `f7/? rdx` - neither a mnemonic nor a `.byte`, so
   silent to the banner AND claiming a length it had not earned. Three
   such markers existed (`f7/?`, `ff/?`, `.0f 0x..`); every path now
   ends at one `undecoded:` label.
 - **That retroactively armed the existing `-rt` check**: with the gap
   put back it fails 1923/1924, having passed for the gap's lifetime.
 - It also found an EMITTER defect: `movsd`'s REX.W is architecturally
   ignored, so every float element access carried a dead prefix bit.

**RCX 140 -> 97** with emit_op's two roles (`tmp`, the staged second
operand; `cpy`, the 24-byte EvalValue copy scratch), 35 sites.

**AND THE LAST FIVE STALE `e.scratch(RCX)` ARE GONE** - all beside a
`bump_counter` that clobbers no register. `scratch()` is a pure
ML_CHECK, and an over-broad one ABORTS A LEGAL FRAGMENT the day the
register is pinnable, so these were a direct blocker on admitting rcx.
One guarded a documented ordering invariant whose justification had
rotted; the ordering stays, the reason is rewritten.

### Remaining for rcx (97)

    26  emit_sync_push_native ) ALREADY SAFE by the existing gates -
    13  emit_ret_native       ) convert last, for the census only
     4  emit_sync_call_inline )
    20  emit_op's residue (StructCtorV, UnaryV, LoadMemberFloat, the
        tag/pointer movabs sites)
     8  jit_compile_chunk
    26  the scattered remainder

Then `data`/`count` allocatable in ElemRead/ElemScratch (the step with
real decline risk - watch `g_jit_elem_noreg`), then rax (357).

**And drive `regcensus.py --raw` (290 hand-encoded lines) to zero** -
it is the more honest metric, since converting one moves a register
from invisible to counted.

## (v) THE ADMISSION TEST, and what it says about rcx today

The census is a proxy. The real question - *may rcx join the pool?* -
has a direct, empirical answer, and it should have been the metric all
along:

    XCACHE_ORDER[] = { 1, 10, 11, 8, 7, 6, 9 }   /* rcx FIRST */
    ./mylang -rt  &&  tests/corpus_diff.sh ./mylang

`take_reg` scans the pool in preference order, so putting a register
first hands it out to essentially every run with a pin - the `--xrot`
insight, used as a gate rather than a differential axis.

**Run today: `-rt` ABORTS at `scratch()` and corpus_diff is 19/23.**
Four programs give WRONG ANSWERS. So rcx is not admissible yet - but
the failure is LOUD and NAMED, which it would not have been at the
start of this arc.

That is the point of the `scratch()` work. Before it there were five
STALE declarations (asserting clobbers `bump_counter` no longer makes -
those would have ABORTED LEGAL FRAGMENTS) and missing ones where real
clobbers happen (those would have MISCOMPILED). Now the assertion fires
at the site.

### The one blocker already located and made loud

`emit_call_epilogue` reloads the caller-saved GP pins and THEN, for a
C1 hoist region, re-derives `rdata`/`rcount` through RCX. Reload, then
clobber - the r9 shape exactly. It now carries `e.scratch(RCX)`, a
no-op today, and the comment names the one-line fix for admission day:
**`HOIST_REGS_MASK` must gain RCX**, so `jit_xcache_clobber` stops a
run with a hoist from spending it. The mask exists precisely so each
contributor names its own registers.

### The order for the rest

1. Run the admission test, read the ABORT, fix that site, repeat. The
   aborts are the worklist; the 19/23 says at least one clobber is not
   even declared yet.
2. `emit_op`'s residue (20), `jit_compile_chunk` (8), `emit_exc_stamp`
   (4), `emit_bake_call_site` (2).
3. `emit_sync_push_native` (26) / `emit_ret_native` (13) /
   `emit_sync_call_inline` (4) are gated by `jit_run_blocks_xcache`
   (CallV / CachedCallV / CallValueV) - safe, and convertible last for
   the census only. **Verify that gate covers `emit_nstack_switch_*`
   (5) before trusting it for them.**

## (w) THE ADMISSION TEST, RUN - one blocker, and it is a DESIGN step

Ran it: `XCACHE_ORDER` with rcx first, `-rt` + `corpus_diff`.

    -rt            ABORT at scratch(), then 1918/1924 with the
                   assert bypassed (6 real failures)
    corpus_diff    19/23 - four programs give WRONG ANSWERS

Then made `scratch()` SURVEY instead of abort, to get the whole
worklist in one run rather than one abort at a time:

    1749 SCRATCH-PIN reg=1        <- rcx
       0 everything else

**One blocker class, and only one.** Every single occurrence is an
emitter using rcx as raw scratch while the pool has handed it out as a
pin. First site by gdb: `emit_branch`'s `tmp_operand`, i.e. the STAGING
role - which is exactly right, and exactly what the threading was for.

### Why this is not a "fix the aborts" job

The clobbers are not bugs. They are declared, correct, and necessary:
the emitters need a scratch register. The defect is structural - the
scratch register is FIXED at rcx, so "rcx is pinned" and "this run
needs staging" cannot both hold.

Two ways out, and they are not equivalent:

 - **(a) a run predicate.** Add RCX to `jit_xcache_clobber` whenever
   the run contains an op whose emit uses staging. Sound, ~10 lines,
   and it makes rcx spendable only for runs with NO int arithmetic,
   compare, or 24-byte copy. That is close to no runs, so it would
   "admit" a register that is never handed out - the boolean-clobber
   mistake of 2026-08-18 in a new costume. **MEASURE the qualifying
   fraction before choosing this**; if it is near zero, say so rather
   than shipping a nominal admission.
 - **(b) ALLOCATE the staging register**, from whatever is free in this
   run, the way `elem_read_idx` allocates the element index. This is
   the real answer and it is what "the real register allocator" means.
   **The threading has already made it a LOCAL change**: every use
   takes the register as an argument now (`tmp`, `cpy`, `keep`, the
   gate helpers' `sc`), so the only edit is where those are BOUND -
   `const uint8_t tmp = RCX;` becomes a pick.

   The care needed is in the candidate set: RDI/R9 are the element
   tiers' roles, RSI/R8 hold the type singletons off-arena, and RDX is
   the divmod scratch. A staging pick must respect all of those, which
   is the same bookkeeping `elem_scratch_reserve` already does - so the
   honest shape is ONE reservation function that hands out every
   emitter role for a run, not a second parallel one.

(b) changes the allocator's structure, so it is the maintainer's call,
not a mechanical continuation - flagged rather than started.

### What the run DID buy

The failure is now loud, named, and enumerable in one pass, where at
the start of this arc it was silent. And the survey - `scratch()`
printing instead of asserting - is the technique to reuse for rax:
it turns "fix aborts one at a time" into a single worklist.

## (x) hand-encoded conversion - the (c) prerequisite, in progress

    290 -> 205 -> 171 hand-encoded lines   (batches 1 and 2)

Both batches byte-identical over 111 corpus programs in both arenas,
-rt 1924/1924, corpus_diff 23/23, objdump oracle clean.

**Read the two numbers TOGETHER.** RAX 360 -> 514 and RCX 99 -> 126
over the same two batches, because a register spelled into a modrm byte
was in NO count before and is in the operand count now. Falling
hand-encoded + rising columns is the conversion working; only the first
number is a target.

### The next batch, and why it needs reading rather than sed

What is left is mostly MULTI-LINE - an opcode byte on one line and its
`u32`/`u8` displacement on the next:

     6  48 8B 80 <u32>   mov rax, [rax+disp32]  -> load_base
     5  48 8B 88 <u32>   mov rcx, [rax+disp32]  -> load_base
     4  83 FE <imm>      cmp esi, imm8
     4  83 B8 <u32> <i8> cmp dword [rax+d], imm8
     4  48 83 FA 01      cmp rdx, 1
     3  FF 09            dec dword [rcx]        -> dec_dword_base
     3  48 C7 C0 <u32>   mov rax, imm32
     3  48 89 E9         mov rcx, rbp
     3  48 83 C4 08      add rsp, 8

A blanket substitution across a line boundary is exactly the shape that
would have mis-mapped a register twice already in this arc (the
LoadElemBool tag scratch, the LoadElem2 ABI argument). Match the FULL
multi-line form, or convert by hand and let vdjcmp prove it inert.

**`dec_dword_base` and `load_base` already exist** - several of the
above need no new encoder, only a call.

## (y) 2026-08-19 - the hand-encoded conversion FINISHED the bulk: 290 -> 21

Batches 3-6, four commits, 149 sites converted plus one new instrument
mode. The count of lines emitting an instruction as literal bytes:

    290 -> 205 -> 171 -> 104 -> 79 -> 59 -> 21 -> 20

The census columns went the OTHER way over the same commits - RAX
360 -> 602, RCX 99 -> 211 - and that is the conversion working, not a
regression. A register spelled into a modrm byte was in no column
before and is counted now. **Only the hand-encoded number is a target;
the columns are a fidelity measure.**

### Batches 3 and 5 were byte-identical. Batches 4 and 6 could not be.

Batch 3 (67 sites) and batch 5 (20 sites) reached encoders that already
existed or that encode one exact form, so `vdjcmp.sh` reported 111/111
byte-identical in BOTH arenas - the strong claim.

Batches 4 and 6 (25 + 38 sites) are the BASE-FORM shapes -
`mov rcx, [rax+disp32]`, `cmp dword [rax+d], imm8` - and they could not
be byte-identical for a reason worth writing down: **the hand-written
form always spelled mod=10 (disp32), while `emit_modrm_disp` picks
disp8 when the offset fits, which every one of these struct offsets
does.** So the choice was "convert and shrink" or "leave 63 registers
invisible". Leaving them invisible defeats #96, so: convert, and say
so. Total -2142 bytes over the corpus, which is a rounding error and is
claimed as nothing more.

### THE INSTRUMENT THAT MADE THAT SAFE: `vdjcmp.sh --same-shape`

Byte identity is the wrong oracle for a change that alters LENGTH. The
new mode normalises exactly three things - the `+ NN:` offset column, a
relative branch target that is the WHOLE operand, and a `frag@+NNNN`
fragment base - and compares everything else: registers, memory
displacements, immediates, tags, helper names. It reports the byte
delta, which is the only reason to make such a change.

It is STRICTLY WEAKER and the script says so in its own report line, so
a result cannot be quoted without its mode. What it cannot see is a
branch that now goes somewhere else - the displacement is exactly what
it erases - so it is paired with the objdump oracle and corpus_diff,
never used alone.

**TWO SELF-TESTS, BOTH WATCHED FAILING**, because a normaliser is
precisely the shape that silently erases what it was meant to keep -
this repo already shipped a sed pipeline that masked so much it called
every program different:
 - NON-VACUITY: `mov rcx` vs `mov rdx` must still differ. Sabotaged by
   adding `s/r[a-d]x/<reg>/g` -> exit 2, named.
 - EFFECTIVENESS: two dumps differing only in offsets and branch
   targets must compare equal. Sabotaged by dropping the branch rule
   -> exit 2, named.

**AND ITS FIRST REAL RUN CAUGHT ITSELF.** The mode reported 39 programs
as "instruction stream, not just lengths"; the instrument was wrong,
not the change - a `frag@+NNNN` base shifts for every fragment placed
after one that changed size. That rule joined shape_norm and the
self-test grew a frag@ line, so dropping it now fails by name. This
mode will be needed again for #101 (the native peephole), which changes
instruction length constantly.

### Two design points the new encoders record

`imul_reg` and `idiv_reg` take ONE operand each, and their comments say
why: rdx:rax is ISA-fixed for both, so the multiplier/divisor is the
only allocatable operand there is. **When the (c) allocator lands those
are `take_fixed` sites by construction**, not oversights to rediscover.

`setcc_r8` REFUSES a register above bl rather than emit the wrong byte
register: with no REX, modrm 4..7 means ah/ch/dh/bh, not sil/dil/spl/
bpl. A caller needing those needs the REX form and should have to say
so. Same principle as `cmp_base_reg` being a separate function from
`cmp_reg_base` - 39 /r and 3B /r are not the same instruction, and
folding them into one "cmp two things" helper is how the operand order
gets silently swapped.

### RSP and RBP joined the Reg enum

Neither is allocatable - rsp is the machine stack pointer, rbp the
fragment's slots base - but `0x48 0x89 0xE9` means `mov rcx, rbp` and
hid rbp from the census exactly as thoroughly as it hid rcx. Naming
them is the same rule that put R8..R11 there.

**Note for the (c) work: the `Reg` enum is declared BELOW the Emitter
class**, so the class body cannot name a register (`call_rax` passes a
bare 0 with the reason in a comment). Move the enum above the class
before the allocator needs to name defaults.

### THE 20 THAT REMAIN, and why each is left

These are not more of the same; each needs a decision, not a pattern:

  2  `49 BB` / `49 BA` + u64      the movabs_r11 / movabs_r10 LAMBDAS
                                  in emit_sync_push_native. Already
                                  named accessors as far as the census
                                  is concerned - converting them is
                                  cosmetic.
  6  F2-prefixed xmm forms        movsd / cvtsi2sd with a disp32 base.
                                  Needs an xmm base-form family; the
                                  existing elem_sd is SIB-only.
  4  SIB forms                    `[rsp]`, `[rsp+0x20]`, `[rcx+r9*8]`,
                                  `movzx eax, [rbx+rax]`. The SIB
                                  encoders exist but take a fixed
                                  scale; these want a general one.
  2  `0F 88` / `0F 8C` + rel32    jcc rel32 inside the shift-count
                                  guard - part of the JUMP machinery,
                                  which owns its own patching.
  2  `48 D3 <modrm>` / `48 C1`    the VARIABLE-count shift: the modrm
                                  is computed from the op, so the
                                  register is already a variable.
  4  scattered                    `push [rcx]`, `lea rcx,[rbx+d]`,
                                  `mov [r9+off], al`, and
                                  `movzx eax, byte [rbx+d]`.

**None of them blocks (c).** The census's blind-spot banner now covers
20 lines instead of 290, and every register in the pin pool's candidate
set is visible. Finish them opportunistically; do not let them gate the
allocator.

### NEXT: approach (c)

The conversion was the prerequisite. The allocator itself is next -
register classes, a per-register capability bitmask, a weight/cost
model, `take_fixed({RAX, RDX})` for the ABI and ISA-mandated cases, and
backtracking / DP where a single pass cannot choose well. See (w) for
why the rcx admission blocker is a design step and not a bug list:
**1749 SCRATCH-PIN reports, all reg=1, none for any other register** -
those are declared, correct, necessary clobbers, and the defect is that
the scratch register is FIXED at rcx.

## (z) 2026-08-19 - APPROACH (c): the register MODEL is built

The maintainer's design, accepted 2026-08-19: *"no fixed scratch
registers anywhere in the codegen. Only classes of equivalent
registers... within a class, a register capability bitmask... the
allocator should first determine the candidates that FUNCTIONALLY could
do the job, then based on a weight system, previous use cases /
availability in the current context, determine which is the most
efficient choice."* Plus `take_fixed({RAX, RDX})` for the cases the ISA
or the ABI names, and backtracking / DP where a single pass cannot
choose well.

### What is built: the MODEL and its self-test

Three questions, kept apart, in `jit.cpp` above the Emitter class:

    RegClass       RC_GP / RC_XMM - never interchangeable at any price
    RegCap         a bitmask of what a register can FUNCTIONALLY do
    gp_weight()    the opportunity cost of spending it

The capability bits are the ISA's inequalities, not ours:

    CAP_BYTE_NOREX    al/cl/dl/bl only - without a REX prefix, modrm
                      4..7 mean ah/ch/dh/bh, NOT sil/dil/spl/bpl. A
                      CORRECTNESS bit, not a size one.
    CAP_MEM_BASE      not rsp/r12 - low 3 bits 100 MEANS "SIB follows"
    CAP_BASE_NODISP   not rbp/r13 - low 3 bits 101 means "disp32, no
                      base"
    CAP_SHIFT_CNT     cl and nothing else
    CAP_CALLEE_SAVED  survives a helper call
    CAP_SYSV_ARG      rdi rsi rdx rcx r8 r9
    CAP_ALLOCATABLE   the allocator may hand it out at all

This IS the maintainer's "RCX can do everything R12 can, PLUS more"
relation, expressed as a superset of bits rather than as extra classes.

The weights: **+4** a SysV argument register (every emitted helper call
rebuilds those, so a value held there is the likeliest to force a
move), **+2** caller-saved (spilled around each call rather than
surviving free), **+1** needs a REX byte. They **reproduce today's
hand-written preference as an OUTPUT** - the cheapest allocatable
register is a callee-saved low one, which is exactly why CACHE_REGS is
spent before XCACHE_ORDER - instead of that order being an assumption
baked into an array.

### Two structural decisions worth keeping

**THE CAPABILITY TABLE IS DERIVED, NOT TYPED.** `base_needs_sib` and
`base_no_base_form` are the predicates the ENCODERS already assert on,
so `gp_caps()` is built from the same expressions. A hand-typed table
would be one more audited table free to drift from the code it
describes - the failure this file has hit five times.

**THE RESERVED SET IS BUILT FROM ROLE CONSTANTS.** `REG_FRAME_ANCHOR`
and `REG_STACK_PTR` join `REG_SLOTS_BASE` as named roles, and
`GP_RESERVED_MASK` is composed from them. A register cannot be freed
for allocation without deleting the role that says what it is FOR.

### THE SELF-TEST, and the arm that was VACUOUS

`jit: the register MODEL` (-rt) re-derives every bit from the encoders,
checks the pool invariants and the allocator's contract. **Watched
failing seven ways**, each message recorded at the test.

**One arm passed the entire suite at first**, and it is the reason to
write them down: dropping rbx from the reserved mask - i.e. making the
FRAME SLOTS BASE allocatable - was green, because the check asked "is
take()'s result in `GP_RESERVED_MASK`", consulting the very constant
the sabotage edits. It asks the ROLE CONSTANTS now. Textbook
oracle-shares-its-subject, found only by doing the sabotage.

### STATE unified, POLICY deliberately NOT

`Emitter::reg_busy` became `RegAlloc::busy`, so the capability-driven
`take()` and the positional `take_reg(pool)` cannot disagree about what
is free; `xclob` became `denied` rather than `busy`, which is what it
always meant (the model can now distinguish "all taken" from "all
forbidden" in a pressure report).

**The pool scan is UNCHANGED.** Switching a site from the pool to the
cost model changes WHICH REGISTER it gets, so that happens per call
site, each one measurable - not all at once behind a refactor. Every
commit in this increment is byte-identical on all 111 corpus programs
in both arenas.

Three copies of the role facts collapsed into the model
(`jit_reg_is_callee_saved` and `elem_reg_usable_nopin` now ask it, two
of them having written the registers as bare numbers). That cost a
cross-check, and the test names what replaced it: the POOL INVARIANTS
cover eleven of the sixteen registers independently, the other five
rest on the SysV ABI.

### WHY BACKTRACKING AND DP ARE NOT BUILT YET

Not a deferral by preference - there is nothing for them to search.
Every allocation decision today has exactly one legal answer, because
the ~1100 remaining hardcoded sites each demand a specific register.
A search over a one-element candidate set is untestable machinery, and
untestable machinery in this emitter is how a wrong register ships.

They become real the moment step 2 below gives `take()` genuine
choices, and the model is already shaped for them: `candidates(need)`
returns the whole legal set as a mask, and `take(need, prefer)` takes a
preference hint - which is the interface a backtracking pass needs to
retry a decision with one register excluded.

### THE ORDER FROM HERE

 1. **`alloc_scratch(caps)`** - the per-op scratch takes a capability
    request instead of scanning {rax, rcx, rdx}. Its callers are few;
    this is the seam the emitter's remaining hardcoding must come
    through.
 2. **Thread the ~1100 unbracketed sites** so they ASK rather than
    assume. The census is the progress bar: RAX 588, RCX 161, RDX 103.
    This is the bulk of the remaining work and it is what unlocks
    everything else - see (w): the rcx admission test reports 1749
    SCRATCH-PIN conflicts, all reg=1, and they are not bugs but
    declared, correct, necessary clobbers of a register that is FIXED
    where it should be requested.
 3. **Widen the pool** - rcx, then rax - now that the model can say
    which sites still claim them.
 4. **Backtracking / DP**, once (2) has made the candidate sets bigger
    than one.

## (aa) 2026-08-20 - alloc_scratch(caps): the seam gets its interface
## and its first caller, and asking finds two things

`alloc_scratch()` had been a DEAD SEAM since it was written - declared
as the route every hardcoded rax/rcx/rdx must come through, called by
NOTHING. Giving it a capability interface alone would have changed
nothing, so this increment also converts the first site.

    int alloc_scratch(need, prefer = 0, exclude = 0)

**`prefer` is the whole mechanism**, and it is worth being explicit
about why: a converted site passes the register it used to hardcode, so
while nothing else wants that register the cost model's discount hands
it straight back and the emitted bytes are UNCHANGED - and the day the
register is pinned, the site takes another one instead of aborting.
That is how rcx and rax reach the pin pool without a flag day.

`exclude` is a LOCAL constraint (it holds our own input; it carries a
status the following code reads), deliberately distinct from `denied`,
which is a property of the whole RUN.

### The first caller was the documented blocker

`emit_call_epilogue`'s hoist re-derive. Its recorded fix was "add RCX
to `HOIST_REGS_MASK`" - deny the WHOLE caller-saved pool to any run
with a hoist region. That is exactly what the clobber mask already had
to stop doing once, for r10/r11: **0 of 20 hoist runs corpus-wide had a
register left**, because "walks an array in a loop" and "may pin a
caller-saved register" had been made mutually exclusive. Asking costs
nothing and does not have that failure mode.

### ⛔ ASKING IMMEDIATELY FOUND TWO THINGS HARDCODING HAD HIDDEN

**(1) THE CACHE-STATE FAMILY'S SECOND MISS.** `ra` is part of "what
currently lives in a register", and it was not in `snapshot_cache` /
`restore_cache` / `clear_cache_state`. A barrier therefore restored the
pin vectors and left the allocator's occupancy at ZERO: `cache` said a
register was pinned while `RegAlloc` said it was free. The first code
to ASK aborted on 14 corpus programs.

This is the *same* miss that family's comment was written about - a run
of `.clear()` calls being an audit table that does not look like one -
hit again by the very next member added to the family. It joins all
four functions now, and **`check_pins_are_busy()`** ML_CHECKs the
invariant wherever either record is rewritten wholesale. `is_empty()`
deliberately does NOT consult it: its two callers (`exit_pc`'s
write-back test, the barrier's brk guard) mean "is a SLOT held in a
register", and a transient scratch is not a slot.

**(2) REGISTER PRESSURE IS REACHABLE.** With rcx in the pool, a
fragment at full pressure holds ELEVEN pins; add rax (the helper
status), the two hoist registers, r12 (no `CAP_MEM_BASE`) and the three
role registers, and nothing is free - three corpus programs reach it.
Today the site is safe only because it hardcodes a register the pool
never spends, which is exactly the assumption being removed.

So `RegAlloc::any_capable()` plus a push/pop **SPILL** - the first in
the allocator, and the "spill to the native stack" half of #96's
mandate. The pair is balanced, so the stack alignment every emitted
call site depends on is unchanged, and it runs only when a hoist region
survives a helper call. It is a SEPARATE call from `take()` on purpose:
spilling costs two instructions and a caller must choose it, never get
it silently because `take()` ran out.

### MEASURED - and the first number was wrong, which is worth recording

Same corpus, same method both sides (rcx placed FIRST in
`XCACHE_ORDER`, `scratch()` reporting instead of aborting):

    before   806 SCRATCH-PIN conflicts, 0 failed compiles
    after    699 SCRATCH-PIN conflicts, 0 failed compiles

**107 removed by converting ONE site** - and the site that most needed
it, since the epilogue runs after every helper call.

The first reading of the "after" number was **288**, which looked like
a 6x win and was an ARTIFACT: three programs aborted early on the
pressure check, so their remaining conflicts were never counted. The
spill made them compile, and the honest number went UP. **A count taken
over runs that DIED is not a count** - the same lesson `vdjcmp.sh`
learned when it reported a crashed run as a difference.

### Where this leaves step 2

The remaining **699** are the other rcx-claiming sites, and they are
now a mechanical worklist rather than a design problem: each is a site
that CLAIMS a register where it could REQUEST one, and each conversion
is byte-identical by construction (pass the old register as `prefer`).
The two hazards this one exposed are the ones to expect again - a
family that must learn about `ra`, and a pressure point that needs a
spill.

## (ab) 2026-08-20 - the rcx worklist is EMPTY and rcx is STILL NOT
## ADMISSIBLE, which is the finding

Converting the four ref-gates and emit_op's staging register took the
admission survey from **806 conflicts to 0**. Then admitting rcx for
real: **4 `-rt` JIT cases fail and two corpus programs print WRONG
ANSWERS** (16_elem2_fused, 17_elem2_divmod_roles).

No `scratch()` assertion fires, and that is exactly the point.

### The survey can only see a DECLARED clobber

`Emitter::scratch(r)` is a DECLARATION - "I am about to use r as raw
scratch". The survey reports the sites that make it. A site that simply
WRITES the register declares nothing and is invisible: `ElemRead`'s
`data = RCX` is a ROLE with a fixed default, and the nested-read tiers
write it without ever calling `scratch()`.

**Same family as the corpus hole one entry up, one level in.** The first
version of the survey measured the wrong PROGRAMS (`-vdj` over files,
missing `-rt`'s in-process ones). This version measures the wrong THING:
declared intent instead of actual writes. Both reported CLEAN for a
register that was not safe, which from an admission test is the most
expensive wrong answer there is.

### What the instrument does about it

`scripts/rcx_admission.sh` no longer stops at the worklist. When the
list is empty it BUILDS THE REGISTER IN and runs `-rt` and
`corpus_diff`, and only that exiting clean prints "ADMISSIBLE". Its
header states the declared-vs-actual limitation so the verdict cannot be
read as stronger than it is. Today it prints, correctly:

    conflicts: 0 ... worklist empty - now testing the register ADMITTED
    -rt         : ABORTED
    corpus_diff :  plain   22/24 agree
    => NOT admissible: ... a site writes the register WITHOUT declaring
       it

### What is left for rcx: the ELEMENT ROLES

`ElemRead` / `ElemScratch` hold `obj=RAX, data=RCX, count=RDX, idx=R9,
val=RDI` as fixed DEFAULTS. `idx` and `val` are already picked from
`ELEM_CAND` through `elem_scratch_plan`; **`data` and `count` are not**,
and `data` is rcx.

So the next step is the one the earlier plan already named: make
`data`/`count` allocated rather than defaulted, from the same
`ELEM_CAND` machinery, watching `g_jit_elem_noreg` for declines. That
also removes rdx's last blocker (`count`), so the two come together.

⛔ AND IT NEEDS THE SPILL DISCIPLINE, NOT THE PREFER DISCIPLINE. These
roles are live across the whole element tier - several instructions, a
bounds check and a branch - not the three-instruction window
`RefScratch` relies on. Their live range crosses jumps, so a push/pop
borrow would be skipped on some path exactly as it would have been in
the store gate. Either they get a genuinely free register from
`ELEM_CAND` (which is what `elem_scratch_plan` already does for the
other two, and what makes this tractable) or the tier DECLINES to the
helper - which it already knows how to do.

## (ac) 2026-08-20 - the ELEMENT ROLES are allocated; rcx still blocked
## by at least one more undeclared writer

`ElemRead` and `ElemScratch` held their roles as FIXED DEFAULTS. `idx`
and `val` were already allocated; **`data` (rcx) and `count` (rdx) were
not, and nothing DECLARED them** - no `scratch()` call, which is exactly
why the admission survey reported a zero worklist while these tiers
wrote rcx.

### What landed

**`elem_read_plan(e, idx)`** - the read tier's allocator, mirroring
`elem_scratch_plan`. It allocates **both** `data` and `count`, and the
asymmetry is the interesting part:

  - the STORE tier's `count` is **ISA-FIXED**: its compound `/=`, `%=`
    arms emit `cqo; idiv`, which reads RDX:RAX and writes the remainder
    to RDX. So `elem_scratch_plan` allocates `data` ALONE.
  - the READ tier emits no idiv - `count` is only
    `_M_finish - _M_start`, shifted and compared - so both are free.

`data` becomes a SIB base, which every `ELEM_CAND` member can be (none
is rsp/r12). Preferred-first, so with an empty pin set the assignment is
today's and the mechanism landed **INERT**: byte-identical on all 112
corpus programs in BOTH arenas, -rt 1925/1925, corpus_diff 24/24.

### What is still wrong, stated precisely

`scripts/rcx_admission.sh 1` still ends **NOT admissible**: the worklist
is 0, but with rcx admitted `-rt` aborts and corpus_diff is 22/24
(16_elem2_fused, 17_elem2_divmod_roles - the same two as before).

So **at least one more site writes rcx without declaring it**, and it is
NOT the element roles - `emit_load_elem2_inline` now takes its whole
role set from `elem_read_plan`. The search should start from those two
programs, and the fastest route is a bisect on the EMITTERS they reach
rather than another grep: the survey cannot help here BY CONSTRUCTION
(that is its documented limit), so the tool to use is the failing
program plus `-vdj`, looking for a write to rcx that no role and no
`scratch()` accounts for.

⛔ **DO NOT "FIX" THIS BY WIDENING THE SURVEY'S ASSERT.** The temptation
is to make `scratch()` fire on every rcx write; it cannot, because the
whole point is that these writes never call it. The durable fix is the
one this entry continues: every register a tier writes becomes a NAMED
ROLE taken from a plan, so "who writes this register" is answerable by
reading the plan rather than by auditing the emitter.

## (ad) 2026-08-20 - the counter bumps were the hidden rcx writer;
## ONE failure left

Fifteen sites bumped a counter with a hand-rolled
`movabs(RCX, &ctr); inc_qword_base(RCX)` and **preserved nothing** -
while the `bump_counter` helper right beside them took rax and
bracketed it with a push/pop. Two idioms for one job, one of them safe.

They were invisible to the admission survey by construction (a bump
declares no `scratch()`), and the emitted result once rcx was pinned
was a hot int local overwritten with **a counter's ADDRESS** - which is
why the failing programs printed a heap pointer where an integer
belonged.

All fifteen route through `Emitter::bump_at` now; the hand-rolled form
is gone, so a new counter cannot reintroduce it by copying a neighbour.
The always-on depth counter gets `bump_counter32_live` (it is a value
the RUNTIME reads, not a statistic, so it must still emit in a release).

**Result: corpus_diff with rcx admitted goes 22/24 -> 23/24.**

⛔ Cost: +460 bytes over the corpus in a TESTS build (2 instructions per
bump). In a RELEASE build `bump_counter` is `#ifdef TESTS` and compiles
to nothing, so the only always-on site is the depth counter, at +2
bytes per emitted sync call. Worth stating because a bump is emitted on
hot paths and the TESTS number looks alarming next to the release one.

### THE ONE THAT REMAINS

`17_elem2_divmod_roles` still diverges (`728167` vs `45737`) and `-rt`
still aborts. The `-vdj` with rcx admitted shows two rcx writes with no
push/pop bracket around them:

    +202: mov rcx, r24.type ; mov rcx, [rcx+0x8] ; cmp rcx, 8
    +306: movabs rcx, 16    ; cmp rax, rcx

Those are `emit_ref_check` and `tmp_lit` - **both of which were
converted** and both of which DO bracket correctly elsewhere in the
same dump. So the question is not "who writes rcx" but **"why did
`reg_holds_pin(RCX)` answer FALSE here"**.

The likely shape, and where to start: `RefScratch` and `tmp_hold` both
ask `e.reg_holds_pin(r)`, which scans `e.cache`. If the pin for this
fragment is recorded somewhere `cache` does not cover at the moment
these emitters run - a barrier having cleared it, or an xcache pin held
outside `cache` - then the borrow is skipped exactly where it is
needed. Note `mov rcx, j` / `mov j, rcx` in that dump is emit_op's
STAGING register, not a pin, so do not read it as one.

⛔ If that is the cause, the fix belongs with `check_pins_are_busy()`:
the invariant "every register `cache` calls pinned is busy in the
allocator" already exists, and its MIRROR - "every register the
allocator has busy is discoverable by reg_holds_pin" - is the one that
would have caught this. Consider asking the ALLOCATOR rather than
`cache` in both borrow sites.

## (ae) 2026-08-20 - two more undeclared writers found; the theory that
## `cache` was the wrong authority was WRONG, and that matters

Entry (ad) predicted the last failure was `reg_holds_pin` consulting
`cache` where it should consult the allocator. **That prediction was
wrong**, and the correction is the useful part.

### The change was made anyway, and it IS right

`Emitter::reg_is_occupied(r)` asks `ra.busy`; `RefScratch` and
`tmp_hold` both use it now. The reasoning stands independently of the
bug: `check_pins_are_busy()` asserts `cache` is a SUBSET of `ra.busy`,
never the reverse, so a register taken by anything other than the pin
machinery - a `take_fixed` pair, a transient scratch - is busy while
`cache` says nothing. A borrow asking `cache` skips its push/pop
exactly where something IS live. Asking `busy` can only borrow MORE
often, which costs two instructions. (`denied` is deliberately not
consulted: a denied register holds nothing, so there is nothing to
preserve.)

It did not fix the failure. Both are true at once, and conflating them
is how a correct change gets reverted later for "not working".

### `emit_div_magic`'s `keep` - the shape worth learning

    /* the dividend is kept aside for the remainder below; naming the
     * role is what lets the allocator move it later */
    const uint8_t keep = RCX;

**A role that LOOKS allocated and is not.** It has a name, and a
comment promising an allocator will move it - and it is a fixed default
that declares nothing. No `scratch()`, so the survey never saw it. It
now borrows like the rest.

It only bites a fragment that BOTH strength-reduces a division AND has
enough hot locals to spend an eleventh pin, which is one program in the
corpus - the reason it outlived every other rcx blocker.

### STILL 23/24 - what the next person should know

`17_elem2_divmod_roles` still diverges. What is now ESTABLISHED, so it
need not be re-derived:

  - rcx IS pinned in the failing fragment (it holds `j`) - but NOT in
    the first fragment, which pins r12-r15. Reading the wrong fragment's
    entry is what sent (ad) down the wrong path; check the fragment that
    actually contains the divmod.
  - the borrows DO fire there - `push rcx` / `pop rcx` are visible in
    the dump around the ref checks.
  - admitting rcx grows the pin budget (MAX_XCACHED 7 -> 8), so the
    fragment pins an EXTRA slot: `mov r8, j; mov rdi, a0` becomes
    `mov rcx, j; mov r8, a0; mov rdi, a1`. The divergence may therefore
    be about the extra pin rather than about rcx specifically - worth
    testing with `MYLANG_JIT_MAXPINS=11` against the rcx build, which
    separates "rcx is unsafe" from "an 11th pin is unsafe".

That last test is the one to run first: it is one command and it halves
the search space.

## (af) 2026-08-20 - MYLANG_JIT_MAXPINS BISECT: it is rcx, not the
## budget - and the clobber is located to ONE instruction pair

The (ae) test, run:

    MAXPINS=4  : elem2dm: 728167 ...   CORRECT
    MAXPINS=5  : elem2dm:  45737 ...   WRONG
    MAXPINS=6,7,8,11, uncapped         WRONG

Pin 5 is the FIRST caller-saved slot, which with rcx placed first in
XCACHE_ORDER is rcx. **So it is rcx itself, not the eleventh pin** - 11
pins is the shipping budget and is fine without rcx. One env var
answered what an afternoon of reading could not; use it first next time.

### The clobber, from the MAXPINS=5 dump (the minimal reproducer)

    +439: movabs rcx, 1000        <-- rcx is the PIN, holding `j`
    +449: imul rax, rcx
    +453: push rcx                <-- the NEXT borrow dutifully saves
    +454: mov rcx, r25.type           the ALREADY-DESTROYED value
    ...
    +473: pop rcx
    +480: mov j, rcx              <-- and flushes the garbage to `j`

A literal staged into rcx and multiplied into rax, with **no borrow**.
Everything around it is correct - the flush/reload bracket the helper
calls properly, and `RefScratch` pushes and pops exactly as designed -
which is why the eye skips over it: the region LOOKS carefully managed.

### What it is NOT (checked, so it is not re-checked)

  - not `tmp_lit` - that borrows now, and its sites show push/pop;
  - not `emit_div_magic` - borrowed in (ae), and its `imul` takes rdx;
  - not the counter bumps - all fifteen route through `bump_at` (ad);
  - not `emit_store_src_gate` / the ref checks - all four borrow.

It is an **IntMulRI-shaped staging** in the specialized-arith family:
`movabs <reg>, imm; imul rax, <reg>`. The emitter for it was not found
before the context ran out; `grep -n "movabs(RCX" src/jit.cpp` lists 15
sites and the arithmetic one is among the later ones, or it is reached
through a helper that takes the register as a parameter defaulting to
RCX (the shape `emit_div_magic`'s `keep` had - a role that looks
allocated and is not).

### The reproducer, in three commands

    # put rcx first in XCACHE_ORDER, build, then:
    MYLANG_JIT_MAXPINS=5 ./mylang tests/functional/17_elem2_divmod_roles.my
    MYLANG_JIT_MAXPINS=5 ./mylang -vdj tests/functional/17_elem2_divmod_roles.my
    # look for `movabs rcx, <imm>` with no `push rcx` before it

Once that one site borrows, re-run `scripts/rcx_admission.sh 1`; it
builds the register in and runs the nets itself, so it will say whether
rcx is finally admissible rather than leaving it to judgement.

## (ag) 2026-08-20 - the `= RCX` grep found the site; REALLOCATION was
## tried there and made it WORSE

The grep works - `grep -n "= RCX" src/jit.cpp` lists the parameter
defaults, and past the four ref gates (all borrowing now) it names the
last one:

    const uint8_t tmp = RCX;      /* the staging / move-aside register */
    const uint8_t cpy = RCX;      /* the 24-byte EvalValue copy walker  */

in the specialized-arith / move emitter. `e.movabs(tmp, in.b_lit())` at
its IntMulRI case IS the `movabs rcx, 1000` from (af).

### The fix that did not work, and why it is worth recording

Allocating instead - `alloc_scratch(CAP_NONE, prefer RCX, exclude
RAX|RDX)` - is byte-identical in the shipping configuration and takes
corpus_diff with rcx admitted from **23/24 to 22/24**. Worse.

The reason was already written down, at RefScratch, and I did not apply
it: **handing back a DIFFERENT register clobbers whatever the caller
left in it, and `ra` cannot warn, because the untracked scratch
registers are not in `busy`.** Excluding rax and rdx is not enough -
rsi, rdi, r8 and r9 carry element-tier roles and type tags at various
points in the same emitter.

⛔ A SECOND, SEPARATE BUG IN THE SAME ATTEMPT, worth its own line: the
first version TOOK the register and never released it. `alloc_scratch`
marks it busy, the emitter is called PER OP, and `ra.busy` persists
across ops - so op 1 got rcx, op 2 got r12, op 3 r13... and EVERY
corpus program's emitted code changed. A take with no matching give is
not a leak here, it is a silent reassignment of every later allocation
in the fragment.

### What this site actually needs

A BORROW, like its three siblings. What makes it harder than they were:
the lifetime spans a whole op emitter with many `return`s and several
branches, so the pop must happen on every path - structurally the same
problem that made `emit_store_src_gate` refuse a borrow until its two
jumps became one.

The shape to reach for is an RAII guard scoped over the `switch`, whose
destructor pops - so no `return` can skip it - or a single exit point.
**Do NOT re-try reallocation**; the comment at the site now says so.

## (ah) 2026-08-20 - the RAII-over-the-switch analysis: SOUND for the
## branches, BROKEN by `return false`

Worked through before writing it, because the change manipulates the
STACK in the hottest emitter and a wrong version is a crash, not a wrong
answer.

### The emitted branches are fine - this part surprised me

The emitter has 86 local patch sites and several `j_slows` / `j_dones`
vectors, so the first worry is a runtime path jumping PAST a pop that a
C++ destructor emits at scope exit. It cannot happen, and the argument
is short:

  - every `j_slows` / `j_dones` vector is declared INSIDE the emitter
    and patched inside it, so no emitted jump leaves the op;
  - every `patch32_here` therefore runs DURING the switch body, while
    the destructor runs AFTER it - so every patch target is at or
    before the pop's eventual position.

A jump can land on the pop or before it. None can skip it.

### `return false` is what breaks it

`return false` means "this emitter DECLINES; the caller emits the helper
instead". A push emitted at the top of the function is already in the
instruction stream at that point, and the caller's helper code follows
it with no pop. **That leaks 8 bytes of stack per execution** - and
unlike a wrong value it compounds until the process dies.

So the guard cannot push at the top. It must push LAZILY - on the first
actual use of `tmp`/`cpy`, which is necessarily inside a case that has
already committed to succeeding.

### The shape that works

A guard whose `arm()` emits the push on first call and whose destructor
pops only if armed:

    TmpBorrow g(e, RCX);        // pushes NOTHING yet
    ...
    case OpCode::IntMulRI:
        g.arm();                // first use: push if occupied
        e.movabs(tmp, ...); op_rr(e, Op::times, RAX, tmp);

`arm()` at each of the ~13 `tmp` and ~7 `cpy` use sites; the destructor
handles every `return true` path, and a `return false` before any
`arm()` pushed nothing. That is the whole change, and it is mechanical
once the sites are listed (they are, in (ag)).

⛔ VERIFY IT WITH THE DECLINE PATH SPECIFICALLY. The dangerous case is
an op that arms and THEN declines - if one exists, the destructor still
pops, which is correct, but confirm no case arms after its last possible
`return false`. `grep -n "return false" ` within the emitter's range
against the arm sites answers it.

Then: `scripts/rcx_admission.sh 1`, which builds rcx in and runs the
nets itself.

## (ai) 2026-08-20 - the lazy arm() guard was BUILT and it does not
## work: RAII scope exit is not the end of the emitted control flow

Built exactly as (ah) specified - `TmpBorrow` with an idempotent
`arm()` emitting the push on first use and a destructor popping - with
21 arm sites inserted across the emitter. It is byte-identical in the
shipping configuration and takes rcx-admitted corpus_diff from
**23/24 to 21/24**. Worse than doing nothing.

### The flaw in (ah)'s reasoning, which I had checked and got wrong

(ah) argued that every emitted jump lands at or before the pop, because
every `patch32_here` runs during the switch while the destructor runs
after. That is true and it is NOT SUFFICIENT.

**The destructor emits the pop after the op's LAST INSTRUCTION - and
that instruction may itself be a BRANCH.** When a case ends with an
unconditional jump (to an exit, to a done label, to the next op), the
pop sits after it as DEAD CODE: never executed, while the push at
`arm()` already ran. Eight bytes leaked per execution.

A C++ scope and an emitted basic block are not the same thing, and RAII
only knows about the first. Every guard shape that ties the pop to
scope exit has this hole, so:

  - RAII over the switch:              broken (this entry)
  - plain push at the top + RAII:      broken by `return false` (ah)
  - reallocation instead of borrowing: broken by caller liveness (ag)

Three shapes, three different reasons, all now recorded.

### What is actually left

The borrow must be tied to the emitted CONTROL FLOW, not to a C++
scope - i.e. the pop belongs immediately after the last USE, on every
path that reaches it, which is what `RefScratch::release()` does
explicitly and what emit_op's callback form does structurally. Both
work because their windows contain no branch.

So the remaining options for this emitter, in order of appeal:

 1. **Per-use windows, like emit_op's.** Convert the ~20 tmp/cpy uses
    to the callback form (`tmp_lit(v, use)`), which places the pop
    right after the consumer. Mechanical, and the pattern is already
    proven in the sibling emitter.
 2. **Give the emitter a single emitted exit** - a label every case
    jumps to, with the pop there. Bigger surgery, but it makes the
    whole family of "borrow across an op" problems go away.
 3. **Leave rcx out of the pool.** 11 pins is the shipping budget and
    it is fine; the measured marginal value of pins beyond the first
    two was ZERO on every bench (see the MAXPINS sweep at (r)). The
    honest question is whether a 12th pin is worth this at all - and
    on the evidence so far it is not. **The value of the work was
    never the pin; it was the four real bugs the conversions found.**

⛔ Do NOT try a fourth guard shape without first deciding (3).

## (aj) 2026-08-20 - the admission FINISHED: both arenas green; what the
## off-arena tail taught

The tracker's admission (docs/jit-optimizations.md #96 (c) + addendum)
ended with three off-arena defects, all silent declines:

1. **alloc_scratch handed out unsaved callee-saved registers.**
   gp_weight's "callee-saved cheapest" is a PIN ranking; for transient
   scratch it selects the one register class whose corruption the
   fragment cannot see - the C caller's. r14 reached the hoist
   re-derive and jit_call_sync_core's frame base died. Scratch now
   excludes callee-saved outright, and the tracker aborts on a write
   to any callee-saved register not in e.saved.

2. **Borrows honour `exclude`, never `denied`.** denied = "a pin here
   would destroy this"; a push/pop borrow preserves it. Honouring
   denied exhausted all 16 registers off-arena.

3. **The reservation went PER-ROLE** (ElemRoleSig). The scalar count
   failed both ways in one day: rcx withheld "for" a capture chain
   that scans ELEM_CAND only (whole max-pin closure fragments dropped,
   every rotation, off-arena - `emit_ok=false` on LoadCaptureV), and
   three withholds for a read whose count role was already safe behind
   the rdx whitelist (the pool starved on the hoisted-read shape).
   The reservation now re-runs each plan's own preferred-then-CAND
   scan with prefs read from the ElemScratch/ElemRead defaults, in a
   FIXED order (never XCACHE_ORDER - a rotation must not change
   emitted code).

**A diagnostic lesson that cost half the hunt:** `grep -B2 FAIL` on an
-rt log shows only failures ADJACENT to the summary - the failure
counts read 6, then 2, then 24, and the first two spawned a
"rotation-dependent" theory that was pure artifact. Count the failure
marker itself (`grep -c 'engaged 0'`).

**State:** rcx is pin 12; -rt 1925/1925 ON and OFF arena x {gcc dbg,
clang dbg, rel-hard, CMake, non-JIT g++/clang, LTO=0}; corpus 24/24 x
{plain, 15 levers, 8 rotations} x both arenas; disasmcheck clean both
arenas; fuzz 40/40; vdjcmp self 112/112. Remaining from the arc:
(y)'s ~20 raw u8() sequences bypass wrote(); rdx/rax admission needs
the same treatment rcx got.

## (ak) 2026-08-21 - rax ADMITTED: the model reaches 13 of 16

The last register, through a two-gate run admission (docs entry "#96
rax" has the full record): run_may_pin_rax's SHAPE whitelist
(accumulator-form arith + loop control + flush-first returns, per the
~2400-hit / ~25-opcode survey under the tracker's new report mode) and
the pick-site COVERAGE gate (every listed op's target actually pinned,
else the generic arm stages through rax). Lever A refuses to arm in a
rax-pinned run (its adapter is `mov rax, pin`). Two standalone emit
wins fell out: LoadImmInt-to-pin and the creg-aware JumpUnlessIntCmp
(every for-loop's entry test).

Reach is NARROW by design - fully-pinned accumulator kernels, 13 int
slots, on-arena only (off-arena rsi carries t_int and the budget is
12) - and that is the honest completion: the GOAL was the model
("13 of 16 registers usable"), the marginal pin's VALUE was measured
near zero long ago (the MAXPINS sweep), and g_jit_rax_pin is the
counter that keeps the reach claim from quietly becoming the rdx
zero-reach story.

What remains on the arc: (y)'s ~20 raw u8() sequences bypassing
wrote(); spill-to-native-stack (the task title's second half, never
started); widening run_may_pin_rax with evidence (immediate-count
shifts want a two-address `sar pin, imm` form first).

## (al) 2026-08-21 - INCREMENT 1 LANDED: the spill-extended hot set

The location map is real: a hot slot's home is now one of
{pin register | native spill slot | frame}, `read_slot`/`write_slot`
are the resolvers that know all three, and up to 16 overflow picks
past the 13 pins live in bare stack qwords (docs entry "#96 INCREMENT
1" for the mechanism, the three watched bugs, and the honest flat
measurement - the two-address family had already collected the
accumulator RMW's win, and 40 slots' worth of frame is 30 cache lines
against a 32KB L1, so the density argument needs a bigger working set
or increment 2's reuse pressure to bite).

What increment 2 (live-range reuse) inherits: the homes as eviction
targets, the seed/flush discipline at entries/exits/barriers, and the
"cache-aware means creg-aware" audit finding - any site it adds must
route slot access through read_slot/write_slot or pair creg with
cspill, because the third home made every hand-rolled creg consult a
stale read.

## (am) 2026-08-21 - INCREMENT 2 LANDED: live-range reuse across seams

The mandate's sentence is now mechanism: a register is REUSABLE by
several variables in the same frame, evicted at seams to the frame
slot (with increment 1's homes taking the un-chained overflow). See
docs "#96 INCREMENT 2" for the two-direction chaining, the
seam-as-linearization-point condition and its side-patched
edge-targeting refinement, and the one honestly-open watched-check
(the stubs' per-pc state). Reach today: phase-structured runs -
sequential loops, init/loop splits; the single-big-loop shapes that
dominate bench/ have no disjoint ranges and correctly plan nothing.

What remains on the arc, unchanged in kind: the ~20 raw u8() tracker
bypasses (y), widening run_may_pin_rax (two-address shifts), and -
now that placement has three tiers - a COST model that decides pin vs
home vs share by weight rather than rank order alone.
