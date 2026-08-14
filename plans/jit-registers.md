# JIT registers: kill the pins, then allocate

Status: **STEP 1 LANDED (the slots base); step 2 (the allocator) next.**
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

## Step 2c - a real fragment register allocator (unchanged plan)

Replace `pick_cached_slots` (which picks at most two int locals by a
heuristic) with linear-scan allocation over the run's live ranges:

  - compute live ranges per slot over the fragment's instruction span
    (the codegen already has `visit_use_def`, the audited use/def
    enumeration - reuse it, do not write a second one);
  - allocate from the free pool; on pressure, spill the range with the
    furthest next use back to its frame slot and reload on demand;
  - at every exit, flush live registers to their slots - `exit_pc`
    already does exactly this for the two cached ones (`flush_cache`), so
    the mechanism exists and generalizes;
  - keep the N5 SOUNDNESS rule: only resolved LOCALS may live in
    registers, never TEMPS. A temp is scratch the VM reuses across run
    boundaries (an int inside one run, a foreach snapshot or slice temp
    between runs), so an eager entry-load/exit-flush would overwrite a
    live container with an int and its type tag. That corruption was a
    FUZZER find, not an -rt one - see plans/archived/native-aot.md N5.

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
