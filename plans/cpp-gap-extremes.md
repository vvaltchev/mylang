# The my/cpp extremes: two causes, not many

Status: **cause 2 FIXED (subscript LICM); cause 1 PARTLY fixed - the
frame pop is 129 -> ~112 Ir, the ref-slot scan is what remains.** Both
2026-08-01.
Investigated 2026-08-01 at the maintainer's request ("why so many cases
> 10x, and why 20-30x?"). All figures callgrind Ir on an `OPT=1
ASSERTS=0` build.

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

## Item 1 - subscript LICM: BUILT 2026-08-01

Landed as `try_hoist_loop_subscripts` (inferencer.cpp), the sibling of the
slice hoister. Design notes kept because two of the obstacles are the
reason the code looks the way it does.

**(a) There was no decl to hoist.** `try_hoist_loop_slices` MOVES an
existing statement, so the value already owns a frame slot. Here the
invariant is a SUB-EXPRESSION, so the pass SYNTHESISES `$licm<N>`:
a fresh slot, an `Identifier` with `sym.kind = local`, a `pInDecl`
`Expr14`, and a rewrite of each occurrence. Allocating a slot post-resolve
needed the enclosing function's frame counter, so `specialize()`,
`specialize_children()` and `try_for_range()` now thread an `int *fsize`
(the root Block's `slot_count` for main, `FuncDescriptor::frame_size` for
a function, null when unresolved) - the same parameter the Inliner's
`walk` already carried.

**(b) A subscript CAN throw; a slice cannot.** Solved by the GUARDED
PREHEADER: the hoisted decl gets the loop's own entry test with the loop
variable replaced by the init value (`COND[k := INIT]`), so a zero-trip
loop evaluates nothing and a >=1-trip loop evaluates it exactly once. The
counted-shape restriction (`for (var k = INIT; k CMP BOUND; ...)` with
both `fr_immutable`) is what makes the guard literally `INIT CMP BOUND` -
no substitution walk, no chance of duplicating a side effect into it.
Two int literals decide it at compile time (false -> no hoist, the body
never runs; true -> no guard emitted).

The ForStmt is left untouched and merely WRAPPED, so `try_for_range` still
matches the counted shape - losing `ForRangeStmt` would cost more than the
hoist gains. That ordering is why the slice hoister works the same way.

### What the -rt suite caught

The first version hoisted `m[1]` out of `for (...) { var m = map(f, a);
s = s + m[1]; }`. `m` is declared INSIDE the body, and `fr_collect_mutated`
deliberately SKIPS a pInDecl assignment - so `m` was tainted by nothing and
`m[1]` looked perfectly invariant. Above the loop its slot is still `none`.
Fixed with `licm_collect_decls` (every name declared in the body: decls,
foreach vars, catch vars, nested func/struct names) plus a reference check.
Worth recording as the general shape: **the mutation sets model REASSIGNMENT,
not DEFINEDNESS**, so any pass that moves code above a loop needs its own
declared-in-body check.

### Calls in the body: precise, not blanket

The first version refused the loop on ANY call that was not a pure user
function - including `len(arr)`. That was too blunt, and the maintainer
rejected it: a const builtin should be fine.

It IS fine, except for the higher-order ones (map/filter/sort/make_array/
make_dict/find), which run a CALLBACK that `fr_collect_mutated` cannot see
(it stops at a `FuncDeclStmt`), so a lambda appending to the base array
would taint nothing and the base would look invariant.

The discriminator is a new inferencer stamp, **`CallExpr::
callable_arg_mask`**: bit i set when argument i's static type is a `Func`
or a `dyn` that might hold one. Stamped in `annotate_hints`, because only
the TYPE question is answerable there - `effective_pure` is the RESOLVER's
answer and does not exist yet when the inferencer runs. LICM then requires
each masked argument to be provably pure: an inline lambda whose
`desc->effective_pure` holds, or a name in `g_fr_pure`. `effective_pure`
is exactly the right proof - a pure function has no capture list, reads
only consts and its own params, and cannot mutate a reference parameter,
so it cannot reach the enclosing frame at all.

Everything else - `len(arr)`, `abs(x)`, `str(v)` - has a mask of 0 and
costs nothing. A callback that is neither an inline lambda nor a named
pure function (a local variable holding one) is unprovable and refuses.

The mask DEFAULTS to `~0u`, so an unstamped call reads as "every argument
may be callable" and declines. That default earned itself immediately:
the resolver's devirtualization swap builds a DirectBuiltinCallExpr
field-by-field and dropped the new field, so every builtin call read as
opaque. Fixed at the root with `CallExpr::copy_call_fields` - ONE place
that enumerates the CallExpr analysis fields, used by `clone()` and all
three swap sites. `vm_len_kind` had been lost the same way once before.

### Measured

All `OPT=1 ASSERTS=0`, both binaries built the same session.

- callgrind Ir, 46_matrix_mult: 187.9M -> 142.7M at scale 1 (**-24.1%**),
  441.2M -> 302.4M at scale 3. Per scale unit that is 126.6M -> 79.9M
  (**-36.9%**), i.e. over 343,000 inner iterations, **369 -> 233 Ir per
  iteration** - the diagnosis above predicted ~200, so the estimate was
  a little optimistic but the mechanism is exactly as scoped.
- wall clock, interleaved A/B: **0.68x** on 46 (0.102s -> 0.069s).
- suite geomean cur/base **0.998x** over 77 benches - neutral, as it must
  be: the pass fires on exactly ONE program in bench/ + samples/.
- output over bench/ + samples/ (83 programs): byte-identical to the
  pre-LICM binary, and VM == tree-walker on every one. Worth stating why
  the usual tw-vs-vm differential is NOT sufficient here: this is an AST
  transform, so both engines run the SAME transformed tree. The oracle
  has to be a binary without the pass.

### Follow-ups not taken

- **`while` loops**: the guard would have to be the condition itself,
  which needs a separate side-effect-free proof. Only `for` is handled.
- **Scalar elements**: `a[i][j]` with both indices invariant and an int
  result is a real (smaller) win, but it needs a typed temp (`th`) and
  interacts with the flat-store paths. The pass descends past it and
  hoists the container half instead.
- **Dict bases**: `base_dict` reads have vivify/`for_write` subtleties.

## Item 2 - the call RETURN path: two increments landed 2026-08-01

Both ideas from the list above were built. The return was 129 Ir; it is
now ~112, and the two call/recursion benches that live on it moved 8-14%.

**(1) `Chunk::plain_frame`.** A DERIVED flag (never serialized - the
loader recomputes it from the three counts it is made of, the same shape
`catch_uids` uses) meaning the chunk owns no per-frame side state: no try
regions, no dict iterators, no dyn iterators. Such a frame provably moved
NONE of the four watermarks between push and pop:

  - `handlers` is only moved by a PushHandler, which exists only in a
    chunk with `n_trys > 0`;
  - the `dict_iters` / `dyn_iters` / `pends` slices are grown by
    push_window ONLY by this chunk's own counts, and every deeper frame's
    pop trims back to its own base, which is >= ours.

So four comparisons became one flag test. A VM_HARDENING build
ML_VM_CHECKs all four are really unmoved - so an op that starts pushing
per-frame state without a chunk count fails loudly rather than leaking it
into the caller's frame. Also removed a dead `cur_seg` store, overwritten
from the new top on every return that had a caller.

**(2) The leave body inlined into its three callers** (`jit_ret`,
`jit_halt`, `vm_leave_call`). Each is already an out-of-line function and
NONE is inside `vm_dispatch`'s loop body - `vm_leave_call` is itself
ML_NOINLINE - so the dispatch text does not grow. The frame this removed
was not small: 13 Ir of prologue + 11 of epilogue against a body doing
~10 Ir of real work, because the `EvalValue res` BY-VALUE parameter is a
32-byte type with a non-trivial destructor, which drags a spill slot and
cleanup scaffolding into the callee. Same disease as
`vm_dispatch_exc_frame` (#82) and the `unique_ptr` parameter lever 1
removed from the PUSH side; same cure.

### What is left, and why it was NOT taken

The `ref_slots` reference-release scan is now **50 of the ~112 Ir**, about
25 per listed slot. That per-slot cost is near-irreducible: it is two
DEPENDENT loads - the slot's type pointer, then its tag - which is the
value model's shape, not this loop's (the N7 arc, not here).

The lever is therefore the LENGTH of the list. In `add_op$0` it holds two
entries, and one of them is an inferred-`int` parameter that can never
hold a reference - the merge only excludes a param whose `decl_type` is
explicitly i/f, because there `bind_param`'s coerce is a RUNTIME
guarantee. Excluding a param on its INFERRED type instead would trade a
memory-lifetime guarantee for the inference stack being airtight, and the
failure mode is silent: a retained reference, visible only as a
lifetime/`use_count` difference, caught only by a hardened build's
re-scan. Worth ~25 Ir per return; deliberately left for a separate
decision rather than folded into this session.

Two smaller safe ones also left on the table: hoisting the per-element
`s >= rec.nslots` bound out of the loop (the chunk knows the total it was
built against), and storing byte offsets instead of slot indices to drop
the x48 per element. Together ~10 Ir - not worth another edit cycle on
the hottest protocol in the VM without a reason to be in there.

**Item 3 (borrowed container args) is untouched** and still belongs with
the N7 arc: binding an array parameter costs an `intrusive_ptr`
retain/release per call, and removing it needs an escape analysis.


## The my/cpp tail, re-measured 2026-08-01 after LICM + the frame-pop work

    35_map_filter        10.0x     63_closures          21.1x
    73_multi_unpack      10.7x     11_closure_counter   21.8x
    34_sort_custom_cmp   13.7x     46_matrix_mult       23.2x
    77_struct_array_lit  16.8x     76_funcval_dispatch  23.4x
    10_recursion_deep    17.9x     75_indexed_unpack    19.8x

Three distinct causes, not one:

  - CALLS - 10, 11, 63, 76. The largest cluster.
  - the VALUE MODEL (boxed EvalValue + intrusive_ptr per container touch) -
    46 after LICM, and the tail of the callback benches.
  - per-op cost in their own ops - 73/75 (unpack), 77 (struct array build).

### THE FINDING: the inline push refuses any call with a reference argument

M5b's fully-inline record push (emit_sync_push_native, jit.cpp) has an
arg-triviality gate - "each arg's current value must be TRIVIAL (the
inline copy is a raw payload copy - a reference needs the helper's
retain)". So a call passing an array/string/dict/struct declines it
ENTIRELY and takes the C++ slow tier, `jit_call_sync_core`.

Measured, same machinery, opposite outcomes:

    10_recursion_deep   sumto(n - 1)   int arg     jit_call_sync_core
                                                   called ONCE in 2.7M
                                                   calls (the first
                                                   descent, by design)
    76_funcval_dispatch fn(st, i)      ARRAY arg   called 1,000,000x -
                                                   EVERY call, 55% of the
                                                   benchmark's Ir

M5b was measured on 10/11/63 - all scalar-arg calls - so its landing
numbers were real but its REACH was never checked. This is the
prove-the-code-ran rule again: "the inline push works" was true of the
shapes it was tested on, and false of the shape most real code has.

FIXED 2026-08-01 - but NOT by an inline retain, which would be WRONG: a
SLICE registers itself in its parent's `slices` set on copy
(SharedArrayObjTempl's copy ctor), so a raw payload copy plus a refcount
bump corrupts that set. The gate is gone and the decision moved into the
copy loop, per argument: a scalar keeps the raw 32-byte copy, a reference
calls `jit_bind_ref_arg`, which runs fast_bind's exact per-argument step.
Measured: 76_funcval_dispatch 1202M -> 1069M Ir (**-11.1%**), the slow
tier 1,000,000 calls -> 1. The remaining top item there is
`jit_subscript` at 274 Ir for ONE general-array element read - the value
model, not the call.

A note for whoever writes the next test here: a small DIRECT callee is
INLINED AWAY by the optimizer, so no call remains to bind. The first
version of `jit_ref_arg_bind` used one and exercised nothing.

### The other half of the call: there is no inline POP

The push is emitted inline (when it fires); the RETURN is still a `call`
into C++ (`jit_ret` / `jit_halt` -> the leave body -> pop_window), ~112 Ir
after the 2026-08-01 work. Symmetric to M5b, and untried. 10_recursion_deep
gets the inline push on ~every call and is STILL 17.9x, which bounds what
the push alone can buy - the remaining per-call protocol is mostly the
return.

### Honest assessment of "can these reach <= 5x"

The two call items above are concrete, measured, and bounded. They will
not by themselves take a 23x bench to 5x: at 5x, 76 would have ~260 Ir per
ITERATION total, and the call protocol alone is ~300 today. Reaching 5x on
the call benches needs the protocol to become a handful of instructions -
i.e. the frame push/pop reduced to what a C++ call does - which is the
whole point of the native-call arc, not one increment of it.
46_matrix_mult and the callback benches additionally need the value model
(N7): a boxed EvalValue + an intrusive_ptr retain/release per container
touch is the floor there, and no amount of call work moves it.


# Re-measured 2026-08-06: the tail is now ONE cause, and it is the
# indirect call

Maintainer's request: run the suite vs C++, take the five worst, find
out why, and re-plan. Suite geomean **2.694x** over 76 paired benches
(`OPT=1 ASSERTS=0`, build-claude/perf). Every figure below is callgrind
Ir per iteration unless stated.

## What the last five days did to the old tail

    46_matrix_mult      23.2x -> 6.25x    LICM + LoadElem2 + inline tier
    10_recursion_deep   17.9x -> 7.56x    C4c inline frame pop
    34_sort_custom_cmp  13.7x -> 10.25x
    35_map_filter       10.0x -> 8.71x
    73_multi_unpack     10.7x -> 9.29x

Both named causes of the 2026-08-01 analysis are now spent: cause 2
(container-read LICM) is fixed outright, and cause 1 (the return path)
gave up what a C++-side edit can give. The new tail:

    1  75_indexed_unpack    20.03x    0.128s / 0.006s   (was 19.8x)
    2  76_funcval_dispatch  19.43x    0.041s / 0.002s   (was 23.4x)
    3  63_closures          19.30x    0.030s / 0.002s   (was 21.1x)
    4  77_struct_array_lit  16.38x    0.063s / 0.004s   (was 16.8x)
    5  11_closure_counter   15.73x    0.024s / 0.002s   (was 21.8x)

Startup is ~1.3ms for MyLang against ~0.3ms for a C++ binary, so
removing it makes every ratio slightly WORSE, not better - the gaps are
real. The C++ twins were audited for fairness in
plans/archived/bench-fairness.md (75 models the refcounted handle bind,
63 and 11 use std::function rather than a dissolvable `auto` lambda).

## The controlled experiment that reframed everything

`s += c()` lowers to a BOXED `compound.v`, while the same code written
`var t = c(); s += t;` lowers to a typed `i.bin`. `compile_int_expr`
(codegen.cpp ~3971) refuses ANY non-builtin call, and says why:

    Builtins ONLY: [...] A user call whose body is tree-walked (a
    closure) would only add the boxing on top of the call (see
    11_closure_counter)

That premise died with #55/M5 - a call is now a native `call`. So the
comment is stale. **But fixing it is worth almost nothing**: the two
spellings measure 414.7M vs 408.4M Ir (**-1.5%**, wall 0.972x), because
the boxed int-int compound already has the #60 fast tier. The 30 Ir of
`vm_num_binop` in 11's profile is NOT this compound - it is IDENTICAL in
both spellings, because it comes from the capture increment inside the
closure. Recorded because the profile genuinely looked like a 15% item
and a controlled A/B said 1.5%.

## Where the time actually goes (measured, not apportioned)

Four probe programs, each differing from the next by one construct:

    q_direct    a small DIRECT call, result accumulated      20 Ir/iter
    q_proto     the same through a CLOSURE value           219 Ir/iter
    q_capture   ... whose body does `start++`              408 Ir/iter
    q_global    ... incrementing a GLOBAL instead          460 Ir/iter
    r_create    creating (not calling) one closure         953 Ir/iter
    r_nocreate  the same loop, no closure                   27 Ir/iter

Three numbers fall out, and they explain all five benches:

**(1) A direct call to a small function COSTS NOTHING - it is inlined
away.** q_direct compiles to `s = s + 1`: no call survives. This is why
08_func_call sits at 1.93x and tells us nothing about calls. Every gap
below is paid ONLY by a call that CANNOT be inlined: a closure, a
func-value, a callback.

**(2) The indirect-call protocol is 219 Ir** against C++'s 2-3 for
`call rdx`. It is already 100% emitted native code - no helper call
appears in q_proto's profile at all - so nothing is left to "nativize".
Reading the emitted code (`-vdj`), one call is ~10 field writes into the
frame record (+0x28..+0x71), a save/switch/restore of rsp onto the
dedicated native stack (M5a), the indirect `call rdx`, and a
three-way `cmp rax` status dispatch. That IS the frame-record model.

**(3) Creating a closure costs ~900 Ir** (953 - 27, of which ~219 is
mk's own call). Per creation: `malloc`+`_int_free` 107, the FuncObject
ctor 54, **the embedded `EvalContext` ctor 39**, `vector<LValue>::
reserve` 34, the handle move-assign 26. C++'s `std::function` with one
captured long allocates NOTHING (small-buffer) and is ~5 instructions.

## Per-bench decomposition

    11_closure_counter    415/iter   protocol 219 + capture store ~190
    63_closures          3092/iter   2 creations ~1800, 3 protocols
                                     ~660, boxed capture compound ~150
    76_funcval_dispatch   898/iter   protocol ~219, `ops[i%2]` boxed
                                     subscript ~180, array-arg ref bind
                                     ~130, FuncObject handle churn ~67
    77_struct_array_lit  3142/iter   make.structarr ~1300 (of which
                                     coerce_struct_field 196 and
                                     malloc/free 214), the two
                                     `row[N].f` reads ~490 FULLY BOXED
    75_indexed_unpack     493/row    unpack helper 96, arr_elem_at 80,
                                     str.len 86 (2 calls), LValue::put
                                     50, SharedStr refcount 48

## The gaps, with reach (static census over bench/ + samples/)

**G1 - the indirect-call protocol (219 Ir).** Reach: `call.val` in 5
programs, `call.v` in 14, plus every callback bench. THE cluster: it is
53% of bench 11, ~21% of 63, ~24% of 76. Not fixable by nativizing
anything - it is the cost of maintaining a VM frame record. Options are
(a) shrink the record for a callee that provably needs less of it, (b)
skip the native-stack switch when already on it (the branch exists; the
save/restore is what costs), (c) inline the RETURN the way M5b inlined
the push - the prior session already flagged this as untried and
symmetric.

**G2 - closure creation (~900 Ir).** Reach: NARROW - `make.closure`
appears in 24 programs but almost always once, at startup; only 63
creates closures in a hot loop. Three separable sub-items, all small
and low-risk:
  - `FuncObject::capture_ctx` is a full `EvalContext` BY VALUE whose
    entire content is `(get_root_ctx(ctx), false, true)` - identical for
    every closure made from the same root, and it holds an empty
    `std::map`. One shared instance per root would do. 39 Ir each.
  - `capture_slots` is a heap `std::vector<LValue>` for typically 1-2
    captures: one malloc + one free per closure. An inline small buffer
    removes both for the common case.
  - Together these are most of the 107 Ir of allocator traffic.

**G3 - `make.structarr` re-boxes and re-validates (~1300 Ir).** Bench 77
builds `[P(i,i+1), P(i+2,i+3)]` by boxing each field into an EvalValue
and running `coerce_struct_field` (196 Ir/iter) per field - although the
struct is POD and every field type is statically proven. #61 already
solved exactly this for `append(arr, S(...))` (construct-in-place) and
for a bare `StructCtorV`; the ARRAY-LITERAL form never got it. Reach: 1
program.

**G4 - `a[i].field` on a flat struct array has NO typed VM path.** The
tree-walker has `member_pod_array_scalar`; the VM has `LoadMemberInt`
(base = a local struct) and `LoadStructFieldInt` (base = a struct-
foreach loop var) - and nothing for a SUBSCRIPT base, so `row[0].x`
lowers to `subscript.v` + `member.v`, both boxed, ~490 Ir of bench 77.
`row` IS inferred `array<P>`, so this is purely a codegen hole, not an
inference one. A clean sibling-case gap of the #92-#96 element matrix.
Reach: 1 program statically (`member.v` occurs only in 77).

**G5 - `str.len` is a helper call at 43 Ir.** Lever 4b gave `len()` on
an ARRAY an inline tier (`arr.len`); the STRING twin still calls
`jit_str_len`, which loads `g_current_ctx`, bounds-checks the slot,
takes a `get_view()` and boxes the result through `LValue::put`. Two
calls are 17% of bench 75. Reach: 6 programs. The precedent and the
shape are identical to the element-load inline tiers.

**G6 - the unpack helper.** 75's `unpack.elem.v` is 96 Ir plus
`arr_elem_at` 80 and `LValue::put` 50 per row. Not yet analysed to the
same depth as the others.

## Recommendation

The honest summary is that **the tail is now one cause with a long
shadow**: an un-inlinable call costs 219 Ir and creating a closure costs
900, and those two numbers ARE benches 11, 63 and most of 76. G3-G6 are
real but each reaches one or two programs.

Smallest useful step, in the maintainer's chosen direction: **G2**. It
is self-contained (two data-structure changes in `FuncObject`, no
codegen, no JIT, no new opcode), it is the largest single measured mass
in the worst-5 that is NOT the call protocol, and it cannot regress a
program that creates no closures. G5 is an equally small second, with an
exact existing precedent to copy.

G1 is the big one and should be planned as its own arc, not an
increment - the prior session's conclusion that reaching <=5x on the
call benches needs the frame protocol reduced to what a C++ call does
still stands, and nothing measured today contradicts it.

## G2 LANDED 2026-08-06: closure creation ~900 -> ~650 Ir

Two data-structure changes in `FuncObject`, no codegen, no JIT emitter, no
new opcode.

**(a) `capture_ctx` -> `capture_root`.** The FuncObject embedded an
`EvalContext` BY VALUE whose entire content was `(get_root_ctx(ctx),
false, true)` and whose ONLY use was to be the parent of the tree-walker's
`args_ctx`. Constructing one cost ~39 Ir per closure and carried a
`std::map` nothing ever wrote. Parenting `args_ctx` straight to the root
is byte-identical: the EvalContext ctor inherits repl_mode / frame /
gfuncs / captures from its parent and the empty node passed all four
through unchanged (its own `captures` was the root's null, and `args_ctx`
overwrites that field one line later); `in_const_eval()` walks to the same
root because the node's `const_ctx` was false; and a name lookup simply
skips one guaranteed-empty map. It is not a new lifetime hazard either -
`capture_ctx.parent` already held exactly this raw pointer. The VM never
used the node at all (it reads only `capture_slots`).

**(b) `std::vector<LValue>` -> `CaptureSlots`.** A hand-rolled tiny vector
with `inline_cap` (2) slots inside the FuncObject, heap beyond that. A
capture list is almost always ONE name - every site in bench/, samples/
and tests/functional/ is exactly one - so the common closure now
allocates NOTHING where it used to pay a malloc, a free and a reserve.

THE LAYOUT CONSTRAINT that shapes it: the JIT walks
`ctx->captures->data()` as a bare `mov r9,[rax+0]` (`emit_ctx_chain_r9`),
i.e. it hard-codes "the first 8 bytes are the data pointer" - which is
where libstdc++ puts vector's `_M_start`. Keeping `ptr` as the FIRST
member is what makes the emitted code byte-identical, and
`layout_contract()` static_asserts it. (A static_assert at namespace scope
cannot name a private member and one inside the class body needs the class
complete; a never-called member function's body is where both hold.) The
other two JIT sites only save/restore the container POINTER into
`rec.caller_captures` and never index it. `CaptureSlots` is neither
movable nor assignable, since `ptr` may point into the object itself.

### The test gap this had to close first

NOTHING exercised either arm. Every capture list in bench/, samples/ and
tests/functional/ is exactly ONE name, and the widest in `tests.cpp` was
TWO - i.e. exactly AT the inline bound, so the heap arm was unreachable
by any existing test, and no test anywhere checked that `clone()` of a
capturing closure is independent. Four `-rt` cases now cover 2 / 3 / 5
captures and clone-independence on both arms.

Both sabotages watched failing:
  - copy ctor copies `ptr` instead of re-pointing at its own buffer (two
    closures share one capture array): ASan `heap-use-after-free`, and
    WITHOUT sanitizers the clone-independence assertions fail outright -
    so the release lanes catch it too, not just the ASan one.
  - `reserve` ignores the inline bound (writes past the 2-slot buffer):
    ASan `heap-buffer-overflow`; without sanitizers, a SIGSEGV on the
    5-capture test.

### Measured (OPT=1 ASSERTS=0 both sides)

callgrind Ir, per iteration:

    r_create (one closure created + destroyed)   953 -> 653   -31.6%
    63_closures                                  618M -> 498M -19.5%
    11_closure_counter                                        +0.00%
    76_funcval_dispatch                                       +0.00%

The two zeros are the point: 11 and 76 create their closures ONCE, at
startup, so a creation-path change cannot help them - and does not hurt
them either. Split by sub-item, (a) was -6.6% of r_create and -4.1% of
63, (b) the rest.

Wall clock, interleaved `--baseline`: **63_closures 0.88x**, suite geomean
cur/base **1.002x** - neutral, as it must be for a pass that fires on one
program. (The my/cpp geomean printed 3.161x in that run against 2.694x
earlier the same day, with no code between them that touches the other 76
benches: the box was slower under a build+valgrind load. That is exactly
the drift the machine-marker note warns about, and why `cur/base` from an
interleaved run is the number to trust.)

Green: `-rt` 1723/1723 in all five modes, rel-hard, clang, CMake, the
corpus differential + the full lever matrix, the non-JIT platform probe on
g++ AND clang++, and 60 fuzzer programs.

### What G2 does NOT touch

The other ~650 Ir of a creation is the FuncObject ctor itself, the
`read_sym` capture snapshot, `jit_make_closure`, and the pooled FuncObject
allocation. And creation is only ~45% of 63_closures; the rest is G1 (three
indirect calls per iteration at 219 Ir each).

## G5 is BLOCKED, and finding out why turned up a correctness bug

**The premise in the G5 write-up was wrong.** `arr.len` is NOT an inline
tier - lever 4b made `len()` a dedicated OPCODE whose emit is still a
`call jit_arr_len`. So there was no inline precedent to copy.

**Where jit_str_len's ~48 Ir actually go** (callgrind, per call, split by
the file each inlined frame came from):

    evalvalue.h   21   the boxed LValue::put of the result
    vm.cpp        14   the helper body + the call frame
    eval.h         8   Frame::at x2
    sharedstr.h    5   get_view()/size() - the actual string access
    basic_string.h 1

So the string access is already cheap; the cost is the call plus the boxed
store. An inline tier removes both - it would read the length and hand it
to the existing ref-aware `store_dst` (which C3/C5 can then elide).

**What blocks it.** An inline read needs the length in a FIELD. `size()`
is `slice ? len : obj->s.size()`, and the non-slice arm exists because
`len` GOES STALE: `TypeStr::append` grows the shared std::string in place
without updating it. Reading `obj->s.size()` inline would mean baking
libstdc++'s std::string layout into the emitter, which the co-located
probe pattern cannot supply (there is no portable way to take the address
of a std::string's size field).

**And that stale `len` is the visible half of a real bug.** See task #123:
`var a = "hi"; var b = a; a += "!";` leaves BOTH at "hi!" (Python leaves b
at "hi"); a `+=` inside a function mutates the CALLER's string; an array
element alias and even `clone()` see the append. README line 299 says
"Strings are immutable like in Python" and line 781 says copies "use
copy-on-write techniques", so this is a spec violation, not a design
choice. Both engines share it (it is in `TypeStr::append`).

**The obvious fix is wrong, and I measured it rather than reasoned about
it.** Adding `&& lval.use_count() == 1` makes EVERY append rebuild,
because the compound path copies the value out of the slot first
(`EvalValue nv = frame->at(target).get()`, vm.cpp CompoundV) - so the
count is structurally >= 2 and never 1. 28_str_concat: **+18268% Ir**,
and a scaling check reads 0.00 / 0.03 / 0.33s at N = 20k / 40k / 80k -
textbook O(n^2). Worth recording as a test trap too: the 50-iteration
"sole owner still appends" test I wrote PASSES under the quadratic
version. Size an asymptotic test to its asymptote.

**The fix worth having (proposed, NOT taken - it is a design change to a
core type):** make every SharedStr a WINDOW, which is what a slice
already is. `len` becomes authoritative for both forms, is resynced after
an in-place append, and the append happens in place only when this
handle's window is the whole current string (`!slice && len ==
obj->s.size()`), else it rebuilds. An alias then keeps its own shorter
`len` and reads the old value - correct for all four repros, with NO COW
clone - while the sole-owner accumulator still appends in place, so the
O(n) idiom survives and no use_count test is needed anywhere. It also
makes `len` a plain field read, i.e. **it is the same change G5 needs**.
Audit list: `size()`, `hash()` (a non-slice uses the StrObj's cached
FULL-string hash, valid only when `len == obj->s.size()`), `append()`,
and every `is_slice()` consumer assuming non-slice == whole string.

The four failing tests are written and were watched failing (1725/1729)
against the current code; they go in with the fix.

## G1 PREPARATION (2026-08-06) - where the 219 Ir actually sit

Not implemented, as instructed. This is the measurement the arc needs
before anyone edits the hottest protocol in the VM.

**The caller side is 77 EMITTED instructions per indirect call**, counted
straight out of `-vdj` for the q_proto probe (one closure call whose body
is `return 1`), split by phase:

    record fill           32   (42%)
    status dispatch       20   (26%)
    arg / target setup     8
    native-stack switch    8
    stack restore          7
    call rdx               1
    depth counter          1
                          ---
                           77

Two phases are two thirds of it, and each has a concrete attack.

### (1) The record fill - 9 of its 32 are already provably dead

The fill writes ~14 fields. THREE of them are the watermark bases, and
they cost 9 instructions because two need a load and one a load + sub +
shift:

    +2518  mov rax,[r8+0x68] / sub / cmp / shr 2 / mov [r10+0x50]   handler_base
    +2543  mov rax,[r8+0x4c]                    / mov [r10+0x54]    diter_base
    +2557  mov rax,[r8+0x50]                    / mov [r10+0x58]    dyiter_base

Those exist ONLY so the pop can trim the watermarks back. But
`Chunk::plain_frame` - the DERIVED flag the 2026-08-01 session added and
which the POP side already trusts (four compares became one flag test) -
proves the callee moves NONE of them: `handlers` is only moved by a
PushHandler, which exists only in a chunk with `n_trys > 0`, and the
diter/dyiter/pend slices are grown by push_window only by the chunk's own
counts, while every deeper frame's pop trims to its own base, which is
>= ours.

So for a plain_frame callee the three fields need not be written at all.
The flag is computed, serialized-free (the loader recomputes it), and
already load-bearing on the other side of the same protocol - this is
using an existing proof one step earlier, not a new one. **~9 of 77
instructions, on every call to a plain-frame callee.** That is the
smallest useful first increment.

### (2) The status dispatch - 20 instructions, mostly cold

After `call rdx` the emitted code runs a three-way `cmp rax` chain
(`-1` = raised, `-3` = a boundary return, else a resume pc) with BOTH
cold arms laid out INLINE between the hot path and the next op. The hot
outcome needs one test. Moving the cold arms out of line (the emitter
already does this elsewhere - the ref-check helper arms, the C5 preheader)
would leave ~3 in the hot path and take the I-cache footprint of a call
site down with it.

### What this does NOT buy, stated plainly

A perfect version of both is ~26 of 77 caller instructions, i.e. the call
goes from 219 Ir to maybe ~180. 76_funcval_dispatch at 898 Ir/iteration
would move ~4%. **Reaching <=5x on the call benches still needs the frame
record itself to become a handful of stores**, which is the 2026-08-01
conclusion and nothing measured since contradicts it: at 5x, bench 76 gets
~260 Ir per ITERATION in total, and the protocol alone is ~300 today.

The structural question the arc has to answer is therefore NOT "which
field can we drop" but "what does a callee that needs no VM frame record
look like" - a leaf, plain-frame, fixed-arity callee whose params are
scalars could in principle run on the native stack with no record at all,
and the record could be reconstructed lazily only if an exception or a
backtrace actually asks for it. That is a design fork for the maintainer,
not an increment.

## #125 RESOLVED 2026-08-06: the VM's caret wins

An index OOB in a TYPED `a[i].f` read carets the SUBSCRIPT, not the whole
member expression. Maintainer's call, and the argument for it: the fault is
the INDEX rather than the field, and it is what every other element read
already reports - struct or not. The tree-walker's MemberExpr span was an
artifact of which node happened to own the carets at that call site, not a
choice: `member_pod_array_scalar` was handed the enclosing MemberExpr's
`start`/`end`. It now takes `sub->start`/`sub->end`.

The bug hid for two reasons worth remembering. In an UNTYPED context the
tree-walker never takes that fast path, so both engines agreed there. And
the 5-mode differential compares thrown exception TYPES, not caret columns -
so the only net that can catch a span regression is an `err loc:` test with
EXPLICIT columns, which this shape did not have. It has one now, and the
sabotage (restoring the MemberExpr span) fails it: expected 56, got 57.

Note the convention when writing such a test: the harness checks the RAW
`loc_end.col`, which is "last char + 2", while the renderer prints
`loc_end.col - 1`. A caret shown as `51:55` is stored as 51..56.

## G6 (#122) analysed 2026-08-06: the premise was wrong; one small win taken

**75_indexed_unpack is now 400 Ir/row** (2004M at scale 1, 5M rows), down
from 493 after G5 removed its two `str.len` calls (-16.98%). Where the 400
go: `vm_unpack_elem_body` 69, `arr_elem_at` 68, `LValue::put` 50, the native
fragment 53, `SharedStr::move_assign` 38, `jit_unpack_elem` 18.

**THE TASK'S PREMISE WAS WRONG.** It said this was "per-op helper cost, the
same shape as the element loads before #92/#93 got their inline tiers" - i.e.
that #93's borrow trick applied. It does not: `vm_unpack_elem_body` ALREADY
borrows the row,

    const EvalValue &elem = outer.get_vec()[outer.offset() + idx].get();

a reference, not a boxed copy. There is no row materialisation to remove,
which was the entire basis for the comparison.

**What actually remains is the VALUE MODEL**: two refcounted `SharedStr`
binds per row. `arr_elem_at` returns `EvalValue(SharedStr(flat_strs()[at]))`
- a handle copy, refcount++ - and `LValue::put` then releases the slot's
previous string (refcount--) and moves the new one in. Not a missing fusion
and not a boxed op that should be typed. The C++ twin models the same
refcounted binds deliberately (bench-fairness class E), so the residual gap
is real but belongs with N7, not with a standalone unpack item.

**The one thing taken:** `vm_arr_elem(elem, k)` re-derived
`elem.get_ref<SharedArrayObj>()` per element although every unpack site
already holds the extracted array - it had to, to check the element count
first. An overload taking `const SharedArrayObj &` removes that per-element
tag-check + cast at four sites (the dyn-foreach N-var unpack,
`vm_unpack_elem_body`'s two loops, and the multi-unpack, whose `get_ref` for
`size()` is now hoisted into a local).

Measured (`OPT=1 ASSERTS=0`, callgrind Ir):

    73_multi_unpack     328.7M -> 321.5M   -2.19%
    75_indexed_unpack  2004.4M -> 1989.4M  -0.75%
    20_foreach_unpack / 74_dyn_foreach_kv / 66_dyn_foreach   -0.00%

The three zeros are correct: 20 takes the flat-int branch and the two dyn
benches iterate a DICT, so neither reaches the boxed element loop.

It is a pure refactor - the same object, the same read - so there is no new
guard to break. The sabotage that DOES mean something is a mix-up between
the outer array and the sub-array, and the suite catches that hard: aborting
with exit 134 under the hardened debug build.

## G1 increment 1 LANDED 2026-08-07 - and the PREPARATION's split was wrong

The first thing this increment produced is a CORRECTION. The G1 PREPARATION
above reported "77 emitted instructions per indirect call", split as record
fill 32 / status dispatch 20 / setup 8 / stack switch 8 / restore 7. Counted
again from `-vdj` against the executed path, and cross-checked against
callgrind (the probe's caller fragment costs 159 Ir per iteration, of which
~23 are the loop's own arithmetic), the real split of the caller side is:

    guards (no mutation yet)   63   (46%)
    record fill + mutations    51   (37%)
    native-stack switch + call 15   (11%)
    status dispatch             5    (4%)
    epilogue (2 type tags)      2
                              ----
                              136

Two corrections matter for the arc:

- **The GUARDS are the biggest phase, not the record fill.** 63 instructions
  re-prove, on every call, facts that are properties of the callee chunk and
  cannot change between iterations.
- **The status dispatch is NOT 20 executed instructions - it is 5.**
  `JIT_RET_SENTINEL == -1` is the NORMAL return, so the hot outcome is
  `cmp/jne/movabs/dec/jmp`; the other ~15 are the two cold arms laid out
  inline. Increment 2 as written ("move the cold arms out of line") therefore
  buys ~ZERO Ir and only I-cache, which - per the guard-elision ceiling rule
  in CLAUDE.md - is not something to push on instruction-count evidence.

### What landed: four precomputes, no new proof obligation

Every piece replaces a per-call recomputation with a value that was already
derivable, so none of them needs a new soundness argument:

1. **`Chunk::plain_frame` replaces three dword compares** (n_dict_iters,
   n_dyn_iters, n_trys) with one byte test. That flag already means exactly
   "owns no per-frame side state", and the POP side has trusted it since
   2026-08-01; this is the identical proof one step earlier. **-4**
2. **`VmStackSeg::cap_slots`.** The segment fit test compared BYTE extents:
   imul the slot sum by 48, then rebuild the vector's length from its two
   pointers - which needed a second register spilled. `slots` is sized once
   and never resized (windows hold raw pointers into it), so its capacity is
   a constant of the segment; the test is now one compare in slot units.
   **-5**
3. **`diter_base` + `dyiter_base` in ONE qword move.** Both are adjacent u32
   pairs (the activation's two mirror counters, the record's two bases), so
   the copy is exact on this little-endian target. The emitter CHECKS the
   adjacency rather than assuming it and falls back to the two-move form.
   **-2**
4. **The `xor r11d, r11d` for the arg-zeroing constant is emitted only when
   there are args.** A zero-arg callee had it dead. **-1**

### Measured (callgrind Ir, `OPT=1 ASSERTS=0`, both binaries this session)

The probe (`q_proto`: one indirect call per iteration to a zero-arg closure
whose body is `return z`) is 2M calls:

    caller fragment   318.0M -> 294.0M   -7.55%   (159 -> 147 Ir/iter,
                                                   i.e. exactly -12 instrs)
    callee fragment   168.0M -> 168.0M    0.00%   (the pop is untouched)
    whole program     496.1M -> 472.1M   -4.84%

bench/:

    10_recursion_deep    372.9M -> 358.1M   -3.99%
    11_closure_counter   415.7M -> 403.7M   -2.89%
    63_closures          497.9M -> 486.5M   -2.29%
    76_funcval_dispatch  898.9M -> 887.9M   -1.22%
    09_fib_recursive      88.3M ->  88.1M   -0.14%

Flat to the last digit, as they must be - none of them reaches this push:
08_func_call, 12_higher_order, 34_sort_custom_cmp, 35_map_filter,
67_make_dict (the callback benches enter fragments through lever 2's
VmInvoker), 01_while_loop, 43_sieve, 46_matrix_mult, 54_mandelbrot,
24_dict_lookup. No vm_dispatch layout tax.

### Sabotage - each one watched failing

- **Remove the `plain_frame` gate** (every callee takes the inline push):
  `-rt` **aborts, exit 134**, `jit_dyiter`'s
  `dyiter_base + i < dyn_iters.size()` assertion - a callee owning dyn
  iterators got no slice.
- **`cap_slots` wrong in C++** (x4): the ML_VM_CHECK added at `push_window`
  fires, so the field cannot drift from `slots.size()`.
- **Point the emitted fit test at `seg_top` instead of `seg_cap`** (it then
  always declines - silently correct, only slower): caught by the EXISTING
  coverage tests, `jit_sync_inline_call: inline path DID NOT RUN`, 1747/1750.
  That is the execution proof for this one: a wrong offset cannot hide behind
  a correct result.
- **Copy only the low half of the fused watermark move**: needs a new test
  (below) - and with it, `-rt` **aborts, exit 134**, `jit_ret_audit`'s
  `dyiters_n == rec.dyiter_base`.

### The new test, and why it had to be built the way it is

A half-copy of the watermark pair is invisible to every existing program,
because the iterator slice is sized at FRAME push from the chunk's own count:
**within one frame the watermark never moves**, so the stale value and the
live value are equal and nothing can tell them apart. The catching shape needs
the SAME record slot reused under two DIFFERENT correct bases, so
`jit: the inline push carries both iterator watermarks (G1)` alternates two
depth-1 callees at the same depth - `A` owns a dyn foreach (dyiters_n is
higher for as long as it runs), `B` owns none - both calling one zero-arg
CLOSURE.

Three shape-eaters had to be defeated first, and they are the reason the test
looks contrived. The callee must be a **closure** (a named function is folded
or inlined away) taking **no argument** (a coercing int/float param makes
`fast_bind` false, which declines the whole inline push), and it must be
called from a **loop** - the record-reuse guard `rec_n != recs_high` means a
FIRST descent always declines to the C++ tier.

### The reach measurement this produced, which the arc should keep

While hunting for a shape that would exercise the fit test, an instrumented
`g_jit_sync_inline` showed **zero inline pushes for ordinary recursion**:

    q_proto (closure called in a loop)     2,000,000 inline pushes
    down(n) with an int param                      0
    down(dyn n)                                    0
    zero-arg self-recursion, 3000 deep             0

Two independent gates cause this: `fast_bind` is FALSE for any callee with an
`int`/`float`-declared param (they need a coercion, not a copy), and the
record-reuse guard declines a monotonically-deepening call chain. So the
emitted push - the thing this increment made cheaper - serves the
**repeat-call** shape, not the **descend-deeper** shape. That is a bigger
finding than the increment: it says the 219 Ir figure the arc quotes is the
INLINE path, and a large fraction of real calls never reach it at all.

### What is left, honestly sized

The guard phase is still 51 instructions and most of it is re-proving
callee-chunk properties. The obvious next increment is a **monomorphic callee
cache** - one 8-byte cell per call site holding the last `desc`; on a hit,
`fast_bind`, the arity compare and the sync-entry test all follow (~13
instructions), leaving a `movabs/cmp/jne`. It is a real inline cache, so it
wants the maintainer's sign-off rather than being folded into a precompute
batch. The design fork stated in the PREPARATION is unchanged and is still
the only route to <=5x: what does a callee that needs NO VM frame record
look like?

## G1 increment 2 LANDED 2026-08-07 - the two-entry callee cache

The guard phase (63 instructions before increment 1, 51 after) spends most of
itself re-proving properties of the CALLEE that cannot change: is the
descriptor `fast_bind`, does its arity match this site, does it have a
compiled chunk with a native sync entry and no per-frame side state. Each is
fixed once the descriptor and its chunk exist - `vm_chunk` is write-once under
`vm_chunk_tried`, `fast_bind` is set with it, `params` is frozen at
`sync_params`, and `sync_entry_off`/`plain_frame` are set by the tier that
compiled the chunk. So a site that watched THIS descriptor pass them once
needs only a pointer compare to re-establish all five.

Per emitted inline call site: one `Chunk::CalleeCache` cell, heap-allocated so
its address (baked as an immediate) cannot move as later sites are added, and
DERIVED - never serialized, so a loaded image's JIT pass builds its own.

**It keys on the `FuncDescriptor *`, not the `FuncObject *`.** Keying on the
object would be one instruction cheaper (it would subsume the type check too)
and WRONG: a FuncObject is refcounted, and a later closure allocated at a
freed one's address would hit an entry describing a different function -
exactly the stale-pointer identity bug CLAUDE.md warns about. A descriptor is
program-lifetime, one of the codebase's stable identities.

### TWO entries, and 76_funcval_dispatch is the reason

The one-entry version was built and measured first. It reads:

    10_recursion_deep    -3.40%
    11_closure_counter   -2.23%
    63_closures          -1.85%
    76_funcval_dispatch  **+0.56%**

76 dispatches through `ops[i % 2]`, so it MISSES on every call and pays the
lookup for nothing - the textbook monomorphic-inline-cache failure, landing on
the one bench this arc is named after. A second entry costs a monomorphic site
NOTHING (entry 0 is tested first, with the same three instructions) and turns
an alternating pair into steady-state hits after two calls. 76 goes
**+0.56% -> -0.90%**.

**The shift order is MRU and that is load-bearing.** The first two-entry
version filled entry 1 and shifted down to entry 0, which put a MONOMORPHIC
callee permanently behind a compare it could never pass: +2 instructions per
call, forever, with every answer still correct. The probe caught it as
138 -> 140 Ir/call. It is now a test (below), not just a measurement.

### Measured (callgrind Ir, `OPT=1 ASSERTS=0`, both binaries this session)

Baseline is increment 1 (10e0b81), so these compound with it.

    probe caller fragment  294.0M -> 276.0M   -6.12%  (147 -> 138 Ir/call,
                                                       i.e. -9 instructions)
    probe whole program    472.1M -> 454.1M   -3.81%

    10_recursion_deep      358.1M -> 345.9M   -3.40%
    11_closure_counter     403.7M -> 394.7M   -2.23%
    63_closures            486.5M -> 477.5M   -1.85%
    76_funcval_dispatch    887.9M -> 879.9M   -0.90%
    09_fib_recursive        88.1M ->  88.1M   -0.09%

Flat (<= +0.06%): 08_func_call, 12_higher_order, 34_sort_custom_cmp,
67_make_dict, 01_while_loop, 46_matrix_mult, 24_dict_lookup, 43_sieve,
54_mandelbrot.

Cumulative for G1 so far: the probe's caller fragment **159 -> 138 Ir per
indirect call (-13.2%)**, 10_recursion_deep **-7.3%**, 11 **-5.1%**, 63
**-4.1%**, 76 **-2.1%**.

### Sabotage - all three watched failing

- **Collapse to one entry** (drop the second compare and the shift) ->
  `the SECOND entry is dead (0)`, 1751/1752.
- **Always hit** (compare rax with itself, so the guards are skipped for every
  callee) -> `-rt` **aborts, exit 134**, `jit_dyiter`'s slice bound. The
  pointer compare is what keeps the skip sound. NOTE the headline still read
  `Tests passed: 1752/1752` - that is the TREE-WALKER pass; the failure is in
  the differential modes and the exit code, the documented VM-only shape.
- **Reverse the shift to LRU** -> `hit arm DID NOT RUN (0)`: every hit of the
  monomorphic site came from entry 1.

The third only became catchable because the two hit arms are counted
SEPARATELY (`g_jit_callee_cache` / `g_jit_callee_cache2`, TESTS builds only;
a release patches both jumps to one address and emits neither). A single
counter cannot see WHICH entry answered, and the answers are correct either
way - so without the split this would have stayed a number in a measurement
log, which is precisely the class of thing that rots.

### A vacuous test caught in the making

The first polymorphic case built its two callees from ONE lambda decl
(`mk(3)` and `mk(8)` of the same `func [k] () ...`). Two closures of one decl
SHARE a FuncDescriptor - and the cache keys on the descriptor - so the site
was monomorphic after all and the second entry was never exercised. The test
now uses two distinct lambda decls. Same family as the shape-eaters already
listed in CLAUDE.md, one level up: the eater here was not an optimizer pass
but the KEY the mechanism uses.

## G1 increment 3 LANDED 2026-08-07 - an annotated param no longer costs the
## whole call

The reach finding from increment 1 said `fast_bind` is false for any callee
with an `int`/`float`-declared param. Measured properly, that is not a detail:

    the same closure called 2,000,000 times, ONLY the annotation differs
      func[z](dyn k) { return z + k; }     742.6M Ir     (371 per call)
      func[z](int k) { return z + k; }   1,194.6M Ir     (597 per call)

**1.61x - annotating a parameter, the thing the language encourages, made
every call to it 61% more expensive.** `jit_call_sync_core` is 180M of that:
the entire inline push declines, because a typed param needs
`coerce_to_decl_type` and the emitted copy loop is a raw copy.

But the coercion is the IDENTITY whenever the argument already holds the
declared type - which is exactly what the inferencer produces for
`func f(int x); f(<int expr>)`. So the question belongs in the GUARD phase
(pre-mutation, where a decline is still possible), not in a per-descriptor
flag.

`FuncDescriptor::bind_req` is that plan: per parameter, the Type singleton
its coercion requires (`t_int`/`t_float`, null for none), populated only when
`fast_bind` is false. The emitted push tests `fast_bind` as before; on the
false arm it walks the arguments and requires each to already hold its
parameter's type, then falls into the SAME copy loop. Everything else - a
widening (bool into int, int into float), a `none`, a wrong type - declines
to the C++ tier exactly as before, correct and merely slower.

**One derivation, four callers.** `fast_bind` was recomputed inline at three
sites (vm_bind_chunk, vm_precompile_all pass A, the -vd splice pass) and now
a fourth thing has to agree with it - the audit-table trap in miniature. So
`compute_bind_flags` (eval.cpp) is the ONE place that reads `decl_type`, and
all four call it, the fourth being the .myv reader: `bind_req` is derived and
not stored, and recomputing `fast_bind` beside it turns the stored byte into
a free cross-check of the param round trip (ML_CHECKed).

**The coercing callee is deliberately NOT cached** (increment 2's cache
re-establishes DESCRIPTOR properties; this one depends on the ARGUMENT
VALUES, which change per call), so it runs the property chain plus ~5
instructions per argument each time - against ~216 Ir for the C++ tier it
used to take.

A trap caught while reviewing the diff: the first version nested the whole
bind plan inside the `if (cache_addr)` branch that emits the cache, so a
site emitted WITHOUT a cell would have lost the `fast_bind` test altogether
and raw-copied a coercing callee's arguments. The plan is now emitted
unconditionally and only the cache store and hit stubs are conditional. It
was latent rather than live (every site currently gets a cell), which is
exactly why it was worth removing.

### Measured (callgrind Ir, `OPT=1 ASSERTS=0`, both binaries this session)

    q_int  (func[z](int k))   1,194.6M -> 602.6M   **-49.56%**
    q_dyn  (func[z](dyn k))     742.6M -> 742.6M    +0.00%
    q_proto (zero-arg)          454.1M -> 454.1M    -0.00%

597 -> 301 Ir per call. The annotated version is now FASTER than the `dyn`
one (602M vs 742M), which is the right order: a typed param also spares the
body its boxed arithmetic.

**bench/ is FLAT on all 14 call-heavy and control benches** (worst +0.06% on
09_fib, inside compile-time noise) - and that is a finding about bench/, not
about the change: NO benchmark annotates a parameter on a call-heavy path, so
the suite cannot see a 1.61x that any annotated program pays. Worth a bench.

### Sabotage - both watched failing

- **Drop the per-argument type check** (accept any argument): the JIT-ON
  differential modes fail, 1534/1535 - a `dyn` float raw-copied into an
  `int` param, where the C++ tier throws. NOTE the headline stayed
  `Tests passed: 1753/1753`: that is the TREE-WALKER pass, the documented
  VM-only shape.
- **Swap the required types** (an `int` param demands t_float and vice
  versa): every call then declines - still CORRECT, only slower, so no
  differential can see it. The counter does:
  `g_jit_bind_coerce is ZERO after the whole suite - the lever is dead or
  untested`.

That second one is the reason the counter exists. A guard that is too strict
is invisible to every correctness net in the project, because declining is
always right.

### The sibling case, TAKEN the same day (see "G1 increment 4" below)

What follows was written when the widening was still a decline; it is kept
because it states the constraint the increment then had to design around.

### The sibling case, measured and NOT taken: a WIDENING argument

`func f(float x)` called as `f(i)` with an int `i` - an ordinary shape, and
one the inferencer accepts - still declines on every call
(`g_jit_bind_coerce` reads **0** for a 30-iteration loop of exactly that,
against 100 for the exact-type twin). So the annotated-parameter penalty is
removed for the exact case and remains in full for the widening one.

Why it was not taken here: a widening (bool -> int, int -> float,
bool -> float) is TOTAL - it cannot throw - so it could be done inline, but
it has to happen in the COPY LOOP, which runs after the record is pushed.
That means the guard phase would have to accept a SET of types per parameter
(exact-or-widenable-or-none) and the copy loop would need a per-argument
conversion arm. Two emitted pieces instead of one, for a case that is
strictly rarer than the exact one. The genuinely un-inlinable residue is
only the NARROWING throw (a float into an `int` param), which must stay a
decline because it raises before the frame exists.

Sized for whoever picks it up: it is the same ~296 Ir per call the exact
case just won back, on the `f(<int>)`-into-`float` shape.

## G1 increment 4 LANDED 2026-08-07 - the widening argument, converted in the
## caller's own temp

Increment 3 left `func f(float x)` called as `f(i)` with an int `i` declining
on every call, and the reason was a real constraint: a conversion in the COPY
LOOP happens after the record is pushed, where a decline is no longer
possible. The way out is that the widenings are TOTAL - bool -> int is a pure
RETAG (a bool's payload is already the int 0/1, the `EvalValue(bool)` ctor
zeroes the whole word), and int/bool -> float is one `cvtsi2sd` - so nothing
about them needs to happen late.

**So they happen EARLY, in the caller's own argument temp**, inside the
guard phase's coercing arm. The copy loop then finds an exact value and stays
the raw copy it always was. Two facts make writing that temp sound:

- `emit_args_range` (codegen.cpp) gives every argument a FRESH temp, compiled
  immediately before the call, so nothing reads it afterwards;
- the value written is precisely what `coerce_to_decl_type` would have
  produced at bind, so a LATER guard declining to the C++ tier still binds
  the right thing - that tier's own coercion is then the identity.

Only the NARROWING (a float into an `int` param) and a non-numeric remain
declines. Those must RAISE, and raising is exactly what cannot happen once
the frame exists.

**The fast_bind path is byte-identical**: no branch was added to the copy
loop, and the whole arm sits behind the `fast_bind` test increment 3 already
emitted.

### Measured (callgrind Ir, `OPT=1 ASSERTS=0`, both binaries this session)

4,000,000 widening calls (2M int -> float, 2M bool -> int):

    widen2   3,182.0M -> 1,978.0M   **-37.84%**   (796 -> 495 Ir per call)

    q_int (exact int param)    602.6M -> 602.6M   +0.00%
    q_dyn (dyn param)          742.6M -> 742.6M   +0.00%
    q_proto (zero-arg)         454.1M -> 454.1M   +0.00%

Fourteen benches flat (worst +0.03% on 09_fib), for the same reason increment
3 was flat: bench/ annotates no parameter on a call-heavy path.

### The register trap, and what caught it

The first version used `rax` as the scratch for the argument's kind byte.
`rax` holds the DESCRIPTOR, which the frame-size read and the cache-hit arm
both still need - **every call died on `Frame::at`'s bounds assert**, in the
debug build, on the first program run. The arm now uses `rsi` (which carries
the fragment's pinned int tag and is overwritten with `total` a few
instructions later, so it is dead-then-redefined) and `r10` (not live until
the record fill). It is the same family as the ABI traps already recorded
here: an implicit register contract, violated by an addition.

### Sabotage - both watched failing

- **Skip the int -> float conversion** (retag only): the extra_check fails
  AND the differential fails, 1534/1535.
- **Accept a NARROWING** (drop the `jne slow` on the int-param arm, so a
  float is retagged as an int): the headline stays 1754/1754 - the
  tree-walker pass - and the two JIT-ON differential modes fail 1534/1535.
  The narrowing must throw and did not.

`g_jit_bind_widen` is bumped ONLY by the two conversion arms, so it separates
"a widening ran inline" from "the coercing push ran" - which
`g_jit_bind_coerce` alone cannot, and which matters for the same reason as
increment 3: a guard that quietly stopped widening would still be CORRECT
(it would decline) and still be counted.

It had to be an **extra_check**, not a `tests` entry: the counter-coverage
assertion runs during the TREE-WALKER pass, before the differential modes
have executed any native code, so a source-table test cannot feed it. The
first attempt was exactly that and reported the counter dead.

## G1 increment 5 LANDED 2026-08-07 - the coercing callee joins the cache

Increment 3 left one loose end it created: a coercing callee was never
CACHED, so it re-ran the five descriptor-property guards on every single
call. Increment 2's cache could not hold it, because a plain hit goes
straight to the push while a coercing one must still run the per-argument
checks - and those are about the ARGUMENT VALUES, which no descriptor match
can re-establish.

The fix is to keep the two kinds of hit APART: `Chunk::CalleeCache` gains a
THIRD entry, `coerce`, tested only after both plain entries have missed. So a
fast_bind site pays for it exactly never on a hit (the two plain compares
still come first) and one compare on a miss.

A coercing hit then skips the whole property chain - the arity compare, the
chunk load and test, the sync-entry test, plain_frame, and the fast_bind test
itself - and only re-derives the callee chunk before falling into the
per-argument checks, which still run every call.

Predicted 10 instructions; measured exactly 10:

    q_int  (exact int param)   602.6M -> 582.6M   **-3.32%**  (301 -> 291)
    widen2 (widening args)    1978.0M -> 1938.0M  **-2.02%**  (495 -> 485)
    q_dyn / q_proto            byte-flat, both

Twelve benches flat (worst +0.01% on 09_fib) - bench/ still annotates no
parameter on a call-heavy path.

The cell's three displacements are measured from the REAL members rather
than written as 0/8/16 (the co-located-probe rule), so a future field cannot
silently shift what the emitted compares read.

### Sabotage watched failing

Point the third compare at a register that cannot match, so the entry is
never hit: **two** nets fire - the widening extra_check's own assertion
(`the coercing callee is NOT CACHED (0)`) and the coverage sweep
(`g_jit_coerce_cached is ZERO after the whole suite`). 1752/1754.

It needs both to be counter-based for the reason increment 3 established: a
cache that never hits is CORRECT - the full chain simply runs - so no
differential, corpus, fuzzer or lever configuration can see it.

## The bench gap CLOSED 2026-08-07: 78_typed_param_call

Increments 3, 4 and 5 were each byte-flat across bench/ - not because they do
nothing, but because **no benchmark annotated a parameter on a call-heavy
path**, so the suite structurally could not see a 1.61x that any annotated
program paid. `bench/my/78_typed_param_call` (with its `.py` and `.cpp`
twins) is that shape: two CLOSURES built over a captured value, one taking
its argument EXACTLY (`int` -> `int` parameter) and one taking a WIDENING one
(`int` -> `float`), each called once per iteration.

Closures on purpose - a direct call to a named function is inlined away at
compile time and would measure nothing about the call protocol.

**Verified to exercise the tiers it exists for**, not merely to run: an
instrumented build reports, at scale 1, 2,000,001 inline pushes, 2,000,002
coercing binds, 1,000,000 inline widenings and 1,999,998 coercing-cache hits
- i.e. every call takes the emitted path, half of them through the widening
arm, and effectively all of them through the third cache entry.

Interleaved A/B against the pre-G1 binary (5ab3428), scale 4:

    base 0.252s -> 0.151s     cur/base **0.60x**  (1.67x faster)
    my/python  0.30x          (3.34x faster than CPython)
    my/cpp    61.4x           (C++ inlines the closure outright - the same
                               class as 76_funcval_dispatch's 0.002s)

The full suite runs clean with it: 77 paired benchmarks, geomean 0.094x
(10.67x faster than CPython).

NOTE for the next person to run the suite: `bench/.bench_cache/` is
git-ignored, so a machine that has not seen this bench will fail fast on the
missing comparison entry and name the `--recompute` command, exactly as it
should for any newly added bench.
