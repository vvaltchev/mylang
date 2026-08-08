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
