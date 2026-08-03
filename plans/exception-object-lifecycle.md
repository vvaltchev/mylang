# Raise by KIND: skip the exception object when it cannot escape

Status: **DESIGNED, NOT STARTED.** Scoped 2026-08-01 while chasing the
residual exception-path cost; parked because the change is invasive and
was not urgent. No code written.

## The observation

On an `OPT=1 ASSERTS=0` build, `bench/my/70_exc_runtime_error.my`
(a divide-by-zero caught in a loop) costs **361 Ir per throw** once the
4.13M startup floor is subtracted. Of that, **44 Ir is the exception
OBJECT's lifecycle**:

    ~7   pooled operator new       (an inlined freelist pop)
    ~13  the constructor
    ~15  the virtual destructor + the backtrace vector's dtor + pooled free
     -   plus the g_vm_jit_exc store and the later move out of it

There is no fat inside those numbers. `Exception` is already minimal - a
vptr, two `const char *`, two `Loc`s, an empty `vector<BacktraceFrame>`,
a bool and an int32 - and `ML_POOL_NEW_DELETE` already reduces the
allocation to a freelist pop/push. Shaving individual stores buys 2-3 Ir.

**The object is built to answer one pointer comparison.** In this shape
it is caught by `catch (DivisionByZeroEx)` with no `as` binding and no
`rethrow`, so nothing ever reads its caret, its message or its
backtrace. It is allocated, constructed, matched, and destroyed 200,000
times to decide one `match_uid() == clause uid`.

## The design

Raise by KIND rather than by object.

Every `DECL_RUNTIME_EX` class already carries a lazily-interned static
uid (`match_uid()`, #74 increment 5). So `vm_catch_match` can match a
`JitRaiseKind` (or a small kind enum covering the DECL_RUNTIME_EX set)
against a clause's `catch_uids` entry with **no object in existence** -
a kind -> uid table is a static array.

Materialize a real `RuntimeException` only where it can ESCAPE. All
three escape points are decided INSIDE `vm_dispatch_exc_body`, i.e.
AFTER the match, which is exactly why this works:

  1. the winning clause BINDS it (`catch (T as e)`, `bind_slot >= 0`) -
     the catch variable needs a value;
  2. the site's `has_rethrow` is set - the exception must be PARKED in
     the region's pend slot for a later `rethrow`;
  3. NO clause matched in this frame - it walks out, and the walk
     captures backtrace frames into it.

In every other case - the overwhelmingly common one - the raise costs a
kind compare and nothing else.

## Why it is invasive (the reason it is parked)

The raise ABI is `std::unique_ptr<RuntimeException>` end to end, and
`g_vm_jit_exc` - the channel every JIT helper conveys through - is
referenced **126 times in vm.cpp and 18 in jit.cpp**. Introducing a
borrowed-vs-owned distinction touches all of them, on the single most
delicate path in the codebase (the one that produced the #78 step-1
infinite loop). It is a focused refactor, not a patch, and it wants a
session with room to re-run the whole battery afterwards.

A narrower first increment, if it is ever wanted without the full ABI
change: give `vm_raise` an overload taking `(kind)` used ONLY by
`jit_raise_kind_exc`'s two arms (div/mod-by-zero, negative shift count),
constructing the object lazily at the three escape points and leaving
every other caller on the `unique_ptr` path. That captures the whole win
for runtime errors while leaving the user-`throw` path untouched.

## Honest ROI

- It helps `70_exc_runtime_error` by roughly **12% of its per-throw
  cost**, and does **nothing** for `42_exceptions`: a user
  `throw Even(i)` genuinely needs an object to carry the struct payload,
  so there is nothing to elide there.
- It is no longer closing a regression. After the `vm_dispatch_exc_hot`
  fix (a03fe95), 70 sits at **+2.04%** and 42 at **+0.71%** versus the
  pre-#78 baseline. This would be new ground, not repair.
- The other large per-throw items, for comparison: `vm_raise` 90 Ir
  (it now contains the inlined dispatch), the native fragment itself
  89 Ir, and the interpreter TRAMPOLINE 33 Ir (the fragment exits with
  the handler pc, the dispatch loop spins one iteration on the
  `EnterNative` marker, and calls back into the same fragment).
  Eliminating the trampoline needs a direct fragment-to-fragment `jmp` -
  never a `call`, which would nest one C frame per iteration and
  overflow the stack on precisely this shape.

## Measurement note

Any attempt at this must be measured at `OPT=1 ASSERTS=0` on both sides
(`bench/run.py` now enforces it via `mylang -v`). The exception path is
one of the places where assertion cost is most unevenly distributed, and
an ASSERTS=1 reading of this area has already been observed flipping the
sign of a result.
