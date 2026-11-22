# The call-protocol arc: why calls are ~20-30x C++

Status: **SCOPED, not started.** Written 2026-08-01. This is the arc that
covers the WORST benches - it is a bigger target than the unboxing work
(plans/archived/unboxing.md), which squarely fixes one bench.

## The benches this is about

    76_funcval_dispatch  23.8x      11_closure_counter  21.8x
    63_closures          23.7x      10_recursion_deep   19.1x

None of them is element access. All four are dominated by what happens
around a MyLang function call.

## Measured anatomy (63_closures)

Per scale unit, scale-3 minus scale-1 so compile time is excluded:

    MyLang 653,600,000 Ir      C++ 21,200,000 Ir      30.8x

Where they go (callgrind at scale 5):

    JIT fragment (real native code)      24.9%
    jit_ret  (the RETURN protocol)       17.6%   <- biggest single item
    EvalValue::operator=(&&)              5.2%
    malloc + _int_free                    6.5%   <- allocation per closure
    LValue::put(&&)                       3.7%
    FuncObject::FuncObject                3.3%
    jit_store_capture_compound            2.7%

## What C++ does that MyLang does not

A C++ lambda capturing one `int` is a **one-word object**, usually kept in
a register or a stack slot. Calling it is a direct call the compiler very
often inlines away entirely. There is no heap allocation, no frame record,
no refcount, no type tag.

MyLang, per closure creation: allocate a `FuncObject`, allocate its
`capture_slots` vector, snapshot the captures. Per call: push a call
record on the activation's segmented stack, bind arguments, switch chunk /
pc / captures. Per return: pop the record, write the parent's result slot,
release any frame slot that might hold a reference.

## The levers, in the order the profile ranks them

### 1. The return protocol (~18% here)

Already optimised three times (the lean leave, the inlined leave body, the
`plain_frame` flag - 129 -> ~112 Ir). What remains is dominated by the
**`ref_slots` reference-release scan**: ~50 of the ~112 Ir, about 25 per
listed slot, two dependent loads each (the slot's type pointer, then its
tag). Narrowing the LIST is the lever - an inferred-`int` parameter sits
in it although it can never hold a reference. That is **task #85, still
open and deliberately so**: it trades a memory-lifetime guarantee for the
inference stack being airtight, and the failure mode is silent (a retained
reference, visible only as a `use_count` difference). See
plans/archived/cpp-gap-extremes.md for the full argument.

### 2. Allocation per closure (~6.5% here)

A closure created in a loop heap-allocates twice per iteration - the
`FuncObject` and its `capture_slots` vector. Two directions:

  - **Pool them.** `PoolAlloc` already exists and is wired into the dict
    node and `PureCache` paths. NOTE the standing warning in
    poolalloc.h: pooling `std::vector` was measured and REJECTED because a
    custom allocator defeats libstdc++'s memmove fast path. A `FuncObject`
    is not a vector, so `ML_POOL_NEW_DELETE` on it is the safe half; the
    capture vector is the part that needs thought.
  - **Prove non-escape and stack-allocate.** A closure that is created,
    called, and dropped inside one frame never outlives it. That is a real
    escape analysis - bigger, and the payoff is the same ~6.5%.

Do the pooled `FuncObject` first; it is small and self-contained.

### 3. Inline the CALL away (the deep one)

The AST inliner already splices expression bodies and small blocks BEFORE
the VM sees them, which is why `fib` and the guard-ternary shapes are fast.
What it cannot reach: a call through a func VALUE (76_funcval_dispatch's
whole point), and a callee that only becomes small AFTER specialisation.

A bytecode-level inliner - splice the callee's chunk into the caller at a
`CallV` whose callee is a known small `native_leaf` - would delete the
record push, the bind, and the return entirely for exactly the shapes that
measure worst. This is the largest and the riskiest item: it interacts
with backtraces (the virtual-frame machinery exists, `inline_frames`), with
the per-frame `PureCache`, and with slot renumbering.

## What to do first

**Instrument before choosing** - the same rule that has now been right
three times in one session (the register ranking was not the constraint;
the container-store flush did not pay; the nested-read fusion has a reach
of one hot site). Specifically:

  1. Split `jit_ret`'s ~112 Ir by component on a live profile, and confirm
     the `ref_slots` scan really is ~50 of it on these four benches - the
     figure came from ONE bench.
  2. Count closure creations per second on 63_closures and 11_closure_
     counter, so lever 2's ~6.5% is attributed rather than assumed.
  3. Only then decide between #85 (a decision, not code), pooling, and the
     bytecode inliner.

## Honest framing

Levers 1 and 2 together are ~25% of these benches - they would take 30.8x
to roughly 23x. **They do not close the gap.** The gap closes only with
lever 3, because C++'s advantage here is not that its call is cheaper: it
is that the call frequently does not happen at all. Any plan that promises
single-digit ratios on these four benches without inlining is wrong.
