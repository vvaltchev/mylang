# The my/cpp extremes: two causes, not many

Status: **DIAGNOSED, nothing built.** Investigated 2026-08-01 at the
maintainer's request ("why so many cases > 10x, and why 20-30x?").
All figures callgrind Ir on an `OPT=1 ASSERTS=0` build.

The suite geomean vs C++ is ~3x, but the tail runs to 32x. The tail is
NOT many separate problems. Every bench above 19x traces to one of two
causes, and one of them accounts for four of the five worst.

## Cause 1 — the call RETURN path (the whole >19x cluster except 46)

`vm_frame_leave` is the #1 or #2 self-cost in EVERY call-heavy bench:

    63_closures          14.5% of total
    11_closure_counter   16.8%
    10_recursion_deep    27.9%
    76_funcval_dispatch  10.4%   (+ jit_call_sync_core 14.2%)

76_funcval_dispatch, 1,000,000 iterations, **1238 Ir per iteration**
against C++'s ~5:

    216  jit_call_sync_core     the call protocol
    165  vm_frame_leave         the frame pop            } 381 per call
     81  the native fragment
    109  TypeArr::subscript     ops[i % 2] + st[0]
     67  jit_store_elem_int     st[0] = ... in the callee
     64  jit_subscript
     36  copy_assign(SharedArrayObj)   refcount churn binding `st`
     26  jit_put_int

So a call whose entire body is `st[0] = st[0] + x` pays **381 Ir of
protocol**. C++'s indirect call is 2-3 instructions.

Worth stating plainly: this is NOT the boxed-vs-typed problem. The typed
instances (`add_op$0`, 4 typed ops) are what the array holds and what
runs - value-template instantiation is working correctly here. The cost
is purely the frame push/pop protocol.

`vm.cpp:5516` already records `vm_frame_leave ~174` from the lever-1
work, so this is a KNOWN cost that lever 1 reduced but did not remove.
The remaining bulk is inside `pop_window()`: the `ref_slots`
reference-release scan, the watermark trims (handlers, dict_iters,
dyn_iters, pends), and the pure-cache stash/restore - work a C++ return
does not do at all because it has no frame metadata to maintain.

## Cause 2 — no loop-invariant hoisting of a container read (46, 32.9x)

`46_matrix_mult` is **369 Ir per inner iteration** (measured by the
scale-1 vs scale-3 delta, so compile time is excluded) for
`s += a[i][k] * b[k][j]`. The inner loop, run 343,000 times:

    12  load.elem.v  r9  = r0[i]     <-- INVARIANT in k, re-read every time
    13  load.elem.i  r10 = r9[k]
    14  load.elem.v  r11 = r1[k]     <-- genuinely varies with k
    15  load.elem.i  r12 = r11[j]
    16  i.bin        r13 = r10 * r12
    17  i.addstep    s += r13; k++ ...

`a[i]` does not depend on `k`, yet pc 12 re-materialises the row 343,000
times. Each one is a boxed `EvalValue` holding an `intrusive_ptr` copy -
`jit_load_elem_value` 130 Ir plus most of `copy_assign`'s 69 Ir, i.e.
**roughly half the inner loop is recomputing something that cannot have
changed**. C++ hoists `a[i]` because it can prove nothing writes it.

MyLang has no loop-invariant code motion for a container read. The
precedent already exists: loop-invariant SLICE hoisting shipped as lever
3 increment 2 (`try_hoist_loop_slices`, inferencer.cpp), with a worked-out
safety analysis - base not in `mut_len`/`mut_content`, bounds immutable,
the COW-detach hazard. This is the same analysis applied to a subscript
that yields a container.

## A measurement trap this investigation hit twice

`__dynamic_cast` (221,404 calls) and `__strcmp_avx2` both appear high in
46's profile and look like RTTI in the hot loop. They are NOT: the counts
are **identical at scale 1 and scale 3** while total Ir goes 188M -> 441M,
so they are compile-time. The same false alarm appeared on the exception
benches. Always compare call COUNTS across two scales before believing a
libstdc++/libc symbol is in a hot loop.

## What to do about it (unstarted, in rough value order)

1. **LICM for a container-yielding subscript** — hoist `a[i]` out of a
   loop that writes neither the base nor the index. Reuses the slice
   hoister's safety analysis. Would take 46 from ~369 to ~200 Ir/iter,
   and helps every nested-container loop (matrix code, arrays of rows).
2. **Cheaper frame pop.** The four worst call benches all live here.
   Ideas, unvalidated: skip `pop_window`'s trims entirely when the callee
   chunk has no handlers/iters/trys (a single precomputed "plain frame"
   flag rather than four separate watermark compares); and skip the
   `ref_slots` scan when the list is empty (fib-class frames already
   short-circuit, but a 2-param frame like `add_op` still scans).
3. **Borrowed container args.** Binding `st` (an array) costs an
   `intrusive_ptr` retain/release per call. A by-reference bind for a
   param the callee never stores would remove it - but that needs an
   escape analysis, so it belongs with the N7 arc, not here.

Item 1 is self-contained and has a shipped precedent. Item 2 is the
higher-value one (four benches) but touches the hottest protocol in the
VM, so it wants its own session and the full battery.
