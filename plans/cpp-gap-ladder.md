# THE LADDER: decompose the my/cpp gap into measured rungs

**Status: AGREED, NOT STARTED (2026-08-15).** Deferred in favour of the
register allocator, which is one of the rungs it would measure. Do this
when the next "where is the remaining gap?" question comes up, and
BEFORE the next speculative optimization increment.

## Why

The overall figure is **my/cpp ~2.33x** (maintainer-measured, H1
2026-08-12) but bench 76 was **~11x**. The gap is NOT uniform: it is a
few outliers over a ~2.3x floor. Framing it as "10x overhead" invites
GLOBAL mechanisms (dispatch, boxing, the value model), and that is
exactly where this project's flat results cluster:

| removed | result |
|---|---|
| memory traffic, allocations, a refcount RMW pair | **win, every time** |
| a mispredicted indirect branch (CGOTO) | **win, ~10%** |
| a PREDICTED branch (guard elision C4d/C4e/C5) | flat, -15..-32% Ir |
| a PREDICTED call to a hot helper (#94 step 3) | flat, -3.3% Ir |
| instructions, by ADDING emitted code (#92, 16B Instr) | **loss, 1.20x** |

The rule is predictive over ~15 attempts and was never applied before
building.

**⛔ AND IT NEEDED REFINING THE FIRST TIME IT WAS USED (2026-08-15).**
Applied to lever A's dead-store elision it predicted a win: the change
removes **-35.84% of 07_nested_loops' data references** and -12.13% of
03_int_arith's. Wall clock: **nothing, suite geomean 0.998x**. The D1
MISS counts were unchanged (50,139 -> 50,164) - every removed access was
an L1 hit to a slot nothing reads, and a dead store to an L1-resident
line retires in the store buffer and stalls nothing.

So the currency is **CACHE MISSES and DEPENDENCY STALLS**, not data
references. The wins in the table above all removed accesses that MISSED
(48-byte elements, freshly allocated objects, refcount RMWs on cold
lines) or sat on a chain something waited for. Before predicting a win,
ask which of those two the removed access was - and if the honest answer
is "neither, it was an L1 hit nobody reads", expect zero.

The reason it was never applied: callgrind counts retired
instructions, a modern core is bound on memory traffic / dependency
chains / mispredicts / front-end, and this box (WSL2) has **no PMU** - so
the deterministic instrument measures the one quantity that does not
bind, and a theory phrased in instructions survives verification while
being wrong.

The ladder replaces intuition about the gap's composition with a
measurement of it.

## The method

For the 3-4 worst my/cpp benches (76_funcval_dispatch first), write C++
variants that each add EXACTLY ONE MyLang property, and time each rung on
this machine, wall clock, interleaved:

1. **plain C++** - today's `bench/cpp` twin.
2. **+ boxed values**: every value a 32-byte tagged union (payload +
   `Type *`), no refcount. Isolates the size/tag cost alone.
3. **+ refcounting** on the reference-typed ones (non-atomic, as
   `intrusive_ptr` is). Isolates the retain/release RMW and its
   dependency chain.
4. **+ frame slots**: values live in 48-byte `LValue`s in a frame array
   rather than in registers. **This is the rung the register allocator
   attacks** - it should be the biggest single delta, and if it is not,
   the allocator's ceiling is lower than assumed.
5. **+ the indirect call** through a function value (76's actual shape).

## What it buys

Each rung's delta is a **wall-clock ceiling** for the class of
optimization that attacks it. After the ladder, "is this increment worth
building?" is arithmetic rather than intuition, and a proposal whose rung
measures 3% cannot be sold as a path to 2x.

## The cheap discipline that pairs with it

Before building ANY perf increment, write down:
 - bytes of memory traffic / allocations / dependency links removed
   **per operation**;
 - emitted bytes **added**;
 - the predicted wall-clock delta.

If the first is zero and the second is not, do not build it. #94 step 3
removed 0 bytes and added ~40 per call site: knowable in advance, and it
measured exactly the predicted nothing (`plans/archived/
inline-borrow-arm.md`).

## RUNG 4 IS MEASURED (2026-08-15) - and it splits into two regimes

`bench/micro/slotcost.cpp` runs 03_int_arith's arithmetic three ways -
values in registers, values round-tripped through a frame slot at
MyLang's real 48-byte stride, and the same plus the `Type *` tag store
MyLang also emits. Intel Core Ultra 9 285T, g++ -O2, 20M iterations:

| loop shape | registers | + slot round-trip + tag | cost |
|---|---|---|---|
| latency-bound (serial `acc` chain) | 3.30 ns | 3.25 ns | **1.00x** |
| throughput-bound (4 indep. accums) | 0.32 ns | 1.03 ns | **3.21x** |

**A frame-slot round-trip is FREE in a loop whose own dependency chain is
long enough to hide it, and costs 3.2x in one with real ILP.**

Corroborated from the MyLang side, three ways:
 - D1 MISSES are ~50,000 on 03_int_arith, 08_func_call AND
   07_nested_loops - **identical**, though their data references are
   14.5M / 6.5M / 5.4M. The count is fixed startup cost; the loops miss
   essentially never. Miss rate 0.35% / 0.79% / 0.93%.
 - 03_int_arith runs **63 instructions per iteration in ~3.89 ns**;
   against the hand-written register-only C++ chain at 3.30 ns, MyLang's
   extra ~45 instructions and 11 slot references cost **0.59 ns total**.
 - which is why removing two of its eight stores per iteration (the
   lever A per-site fix) measured **-35.84% data references and 0.998x
   wall clock**.

### What this means for the register allocator

Its payoff is **not** "MyLang does 336x the data references of C++". It
is `(fraction of hot loops that are THROUGHPUT-BOUND) x (up to 3.2x on
those)`. On a latency-bound loop it is worth exactly nothing, and
03_int_arith - the shape with the most slot traffic per iteration - is
latency-bound.

**The cheap discriminator is already in the bench table**: a benchmark
whose my/cpp ratio is near the ~2.3x floor is latency-bound (MyLang's
traffic hides in the same chain C++ pays), and one far above it -
76_funcval_dispatch at ~11x - is where the traffic is NOT hidden. That
is the same "the gap is not uniform" observation this file opens with,
now with a mechanism behind it.

**So: before building the allocator, rank the corpus by my/cpp and run
this census on the WORST ones, not on the ones with the most slot
references.** Those are different sets, and the whole reason the old
plan pointed at 03_int_arith is that nobody had separated them.

## THE CENSUS OF THE WORST BENCHES (2026-08-15) - it is CALLS, not slots

### Step 1: rank by my/cpp, STARTUP-CORRECTED

The raw bench table's my/cpp is misleading at the top: its worst entries
have cpp times of 1-2 ms, against which MyLang's ~1 ms of parse + compile
+ JIT is a large fraction. Taking the scale-3 minus scale-1 wall time on
BOTH sides cancels that (and needs no iteration count, since the ratio of
deltas is the loop-only ratio). It reorders the list substantially:

| bench | raw my/cpp | **loop-only** |
|---|---|---|
| 30_str_index_iterate | 9.44x | **27.36x** |
| 63_closures | 11.86x | **17.57x** |
| 76_funcval_dispatch | 10.68x | 11.40x |
| 11_closure_counter | 7.99x | 11.11x |
| 64_struct_create | 8.08x | 10.26x |
| 75_indexed_unpack | 8.68x | 9.18x |
| 35_map_filter | 7.11x | 8.74x |
| 73_multi_unpack | 8.91x | 8.47x |
| 03_int_arith | 1.36x | **1.18x** |
| 09_fib_recursive | 6.41x | **0.09x** (FASTER than C++) |

03_int_arith is at PARITY once startup is removed - which independently
confirms the rung-4 finding that its eleven slot references per iteration
cost nothing. 09_fib_recursive BEATS C++ (the recursion unroll plus the
per-frame pure-call cache against a naive recursive twin).

### Step 2: classify each worst loop's emitted body

| bench | instrs | calls | slot ld | slot st | dominant |
|---|---|---|---|---|---|
| 30_str_index_iterate | 30 | 1 | 2 | 0 | **CALL** |
| 63_closures | 151 | 6 | 15 | 7 | **CALL** |
| 76_funcval_dispatch | 248 | 6 | 11 | 4 | **CALL** |
| 11_closure_counter | 259 | 15 | 18 | 6 | **CALL** |
| 64_struct_create | 137 | **0** | 22 | 14 | **slot traffic** |
| 75_indexed_unpack | 92 | 5 | 4 | 4 | **CALL** |
| 73_multi_unpack | 55 | 1 | 2 | 0 | **CALL** |

**SEVEN OF THE EIGHT WORST BENCHES ARE CALL-DOMINATED.** Exactly one -
64_struct_create - is slot-traffic-dominated, and it makes no calls at
all.

30_str_index_iterate, the worst at 27x, is the clearest: its whole inner
loop is `lea` two slot addresses, ONE helper call for the fused
`ord(s[i])`, two `movabs` to reload the type-tag singletons the call
clobbered, and the accumulate. **6.3 ns per character against C++'s
0.23** - a function call where C++ does a load.

### The conclusion for the register allocator

It addresses slot traffic. Slot traffic dominates ONE of the eight worst
benches. On the shape the old plan was written around (03_int_arith) the
same measurement says the loop is already at C++ parity.

**64_struct_create is the single honest candidate** - 36 slot accesses in
137 instructions, 9.85 ns/iteration, IPC ~2.8-3.5, so partly but not
wholly latency-bound. Worth a targeted experiment; not worth a general
allocator on its own.

**What the census actually points at is the CALL - its argument setup,
its clobbering of the pinned type-tag registers, and the work in the
helper body.** That is the same family as the wins already in the
record (the nested-read fusion, the element-store inline tiers, the
struct baked layout): replace a helper call with emitted code that does
the work. 30_str_index_iterate is the cleanest instance left.

