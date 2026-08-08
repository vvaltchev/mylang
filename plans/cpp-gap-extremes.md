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

Item 2 is the higher-value one (four benches) but touches the hottest
protocol in the VM, so it wants its own session and the full battery.

## Item 1 in detail — subscript LICM, scoped 2026-08-01

I started this and stopped at the design, because it is NOT the small
extension of the slice hoister it looks like. Two obstacles, both real:

**(a) There is no decl to hoist.** `try_hoist_loop_slices` moves an
EXISTING statement - `var sl = base[a:b];` - so the hoisted value already
owns a frame slot and an `Identifier` with a resolved `sym`. In 46 the
invariant is a SUB-EXPRESSION (`a[i]` inside `s += a[i][k] * b[k][j]`),
so the transform must SYNTHESISE a temp: allocate a fresh frame slot,
build an `Identifier` with `sym.kind = local` + the right `th`/static
type, emit `var $licm0 = a[i];` above the loop, and rewrite each
occurrence. Allocating a slot post-resolve needs the enclosing
`FuncDeclStmt` (to bump `frame_size`, capped at 64 like the tail
inliner's local remap) - and `specialize()` is a free function with no
current-function handle. So it needs threading, or the pass has to move.

**(b) A subscript CAN throw; a slice cannot.** The slice hoister is sound
for a ZERO-iteration loop precisely because slicing only clamps
(`base_sliceable` + int bounds => cannot throw), so moving it before the
loop cannot introduce an error. `a[i]` throws on OOB. Hoisting it out of
a loop that runs zero times turns "never evaluated" into "throws before
the loop" - observable, and wrong. So it needs EITHER a proof the loop
executes at least once (for a counted `for` with literal/immutable
bounds this is often provable), OR a proof the index is in range.

Neither is a blocker, but together they make this a real optimizer
change rather than a 30-line extension, and it should start fresh.

### The soundness fix: a GUARDED PREHEADER

Obstacle (b) is solvable and this is the design to build. The hoist is
unsound only when the loop may run ZERO times, so give the hoisted decl
the loop's own entry condition:

    for (var k = 0; k < n; k++)  s += a[i][k] * b[k][j];

becomes

    Block(scope_free) {
        if (0 < n) { var $licm0 = a[i]; }        // the GUARD
        for (var k = 0; k < n; k++) s += $licm0[k] * b[k][j];
    }

The guard is the loop's `cond` CLONED with the loop variable substituted
by the init's rvalue - for `for (var k = INIT; COND; ...)` it is
`COND[k := INIT]`, here `0 < n`. Then:

  - loop runs >= 1 time  -> the guard is true, `a[i]` is evaluated
    exactly once, exactly as before (it was evaluated on iteration 1);
  - loop runs 0 times    -> the guard is false, `a[i]` is NOT evaluated,
    so an OOB `i` still throws never. The observable behaviour is
    identical, including which errors occur and in what order.

`$licm0` is only READ inside the loop body, which the guard proves is
only entered when the decl ran - so the "declared in an inner block" is
not a problem (its slot is frame-wide; resolution is by slot).

Cost when the loop does run: one extra comparison before the loop,
against 343,000 saved boxed reads on 46.

CRITICAL: the guard must be built WITHOUT restructuring the `ForStmt`
itself. An earlier idea - moving `init` out and leaving `for (; cond;
inc)` - would stop `try_for_range` from matching the counted-loop shape,
losing the `ForRangeStmt` specialization, which is worth more than the
hoist. The wrapper Block + an untouched ForStmt keeps that intact (the
slice hoister already relies on this ordering).

### What remains to build

1. **Thread a frame-size pointer through `specialize()`** exactly as the
   Inliner's `walk(slot, depth, int *fsize, no_block)` already does
   (resolver.cpp:3130) - `(*fsize)++` is the fresh-slot allocator, and
   `FuncDeclStmt::desc->frame_size` is the per-function counter. Cap at
   64 like the tail inliner. `specialize()` (inferencer.cpp:5178) is
   currently a free function with no such parameter.
2. **Find the candidates**: a `Subscript` appearing in a DIRECT statement
   of the loop body (unconditional per iteration - NOT nested in an `if`
   or an inner loop, or hoisting could evaluate what never would have),
   whose base and index are both loop-invariant by the existing
   `fr_collect_mutated` / `fr_immutable` analysis, and whose static type
   is a CONTAINER (hoisting a scalar buys nothing - the win is the
   `intrusive_ptr` copy).
3. **Synthesise** the `Identifier` (fresh uid `$licm<N>`,
   `sym.kind = local`, the new slot, `th`/static type copied from the
   subscript) and the `Expr14` decl, plus the guard `IfStmt`.
4. **Substitute** every occurrence of the subscript in the body with the
   temp - `for_each_child_slot` already does this shape of rewrite for
   the inliner's param substitution.
5. **Tests**: the zero-trip case (an OOB index in a loop that never runs
   must still NOT throw - this is the whole point of the guard), a
   mutated base, a mutated index, an aliasing write through the hoisted
   row, plus a `-vd` shape pin and the differential.

### Session note (2026-08-01)

Design complete, implementation NOT started. Stopped deliberately: the
change is ~200 lines across the specialize pass plus the fsize threading
and its tests, and it was scoped at the end of a long session with too
little room to land it and run the full battery. Starting it and leaving
it half-applied in the optimizer would be worse than not starting.

A genuinely small down-payment, if a partial win is wanted first:
extend `try_hoist_loop_slices` to accept a `Subscript` rvalue (not only
a `Slice`) that yields a CONTAINER - i.e. hoist an explicit
`var row = a[i];` written by the user in the loop body. Same safety
analysis, same transform, no synthetic temp, no slot allocation. It does
NOT help 46 as written (which has no such decl), but it is the correct
first half and it makes the hand-written form fast.
