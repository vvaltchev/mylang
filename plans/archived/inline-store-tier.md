# The inline element-STORE tier (task #92)

## Why

Profiling on 2026-08-02 (`OPT=1 ASSERTS=0`, callgrind, JIT on):

    43_sieve   452.7M Ir  vs  16.7M for -O3 C++   = 27.2x, the worst bench

      jit_store_elem_int (self)            212.3M   46.9%
      the JIT'd native code itself          89.0M   19.7%
      SharedArrayObj::size()                37.5M    8.3%
      sharedarray.h inlined into the helper 31.2M    6.9%
      intrusiveptr.h into the helper        12.5M    2.8%
      evalvalue.h into the helper            9.4M    2.1%
      clone_aliased_slices                   9.4M    2.1%

**One helper and its inlined headers is 66% of the program**; the native
code is 20%. Per operation that is **85 Ir to store one bool**, where C++
emits one byte store. The JIT emits native code and then calls a helper
that redoes the whole managed model: type tag, storage kind, const +
readonly, negative wrap, `size()` TWICE, slice check, `use_count()`, the
store, `invalidate_hash()`.

Two side findings from the same profile: `size()` appears as its OWN
symbol (8.3%), so it is not being inlined into the helper at all; and
`clone_aliased_slices` runs on the hot store path.

## LANDED 2026-08-02

    bench                    before        after   delta    my/cpp
    43_sieve            452,745,568  175,222,664  -61.3%   27.2x -> 10.5x
    14_array_subscript  181,497,449   94,600,524  -47.9%   10.0x -> 5.2x
    46_matrix_mult      122,046,979  121,484,093   -0.5%   23.4x -> 23.3x
    18_foreach_array    158,367,308  158,448,632   +0.1%    6.0x -> 6.1x

**88.9 Ir saved per element store.** `jit_store_elem_int` is GONE from the
sieve's profile - it is now 82.8% native code, and what is left is COMPILE
time (`dynamic_cast` + `strcmp` in the optimizer), not the program. The two
read-heavy benches are neutral, as expected: this is the store side.

The guard that made it work is `has_slices`, NOT `use_count() == 1` - see
the root-cause section below, which is why the first attempt was dead.

### What is proven, and what is not

PROVEN load-bearing (removed, watched the test fail):
  - the **COW guard**. Without it a live slice observes a value it should
    have been cloned away from: `7007` against the tree-walker's `1007`.
    Silent, and the whole 1690-test suite passed with it removed until the
    case was built correctly - see the test-trap note at the end.

NOT proven by an isolated case, and recorded rather than claimed:
  - the **slice-base** guard (storing THROUGH a slice, where elements live
    at `shobj->off`);
  - the **readonly** guard.
  Both are emitted and both match the interpreter's own conditions
  (`!arr.is_slice()`, `!arr.is_readonly()`), and the whole differential is
  green with them in - but every shape built to reach a JIT-COMPILED store
  through them either declined earlier for another reason or made the VM
  error where the tree-walker did not. The latter is worth its own look:
  it is either an invalid program or a separate divergence.

## PREP - clone in C++, resume native (landed 2026-08-02)

The maintainer's design: instead of DECLINING every store while a slice
exists (85 Ir each, forever), the COW guards call `jit_store_elem_prep` -
the clone ALONE - and jump back to the fast path's retry head. The
interpreter pays the clone once and stores raw ever after; now the emitted
code does too.

The rules, each load-bearing and each sabotage-verified:

  - prep runs only AFTER the const + readonly guards: the interpreter
    throws on those WITHOUT cloning, and a clone is observable via
    `intptr`. Proven by inverting the order and watching the readonly
    case's prep=0 assertion fire - but ONLY once the test's slice used a
    `runtime()` index: `C[1:4]` with literal bounds const-folds at parse
    time, no live slice ever exists, and the first ordering sabotage
    PASSED vacuously.
  - prep bounds-checks in C++ BEFORE cloning: an OOB store must not
    detach anything (the interpreter checks bounds first).
  - prep clones only the OVERLAPPED index's slices, never all: early
    detach of a non-overlapping slice is observable via `intptr`. Such a
    store stays on the slow helper until the loop reaches the slice's
    range - exactly the interpreter's per-store behaviour.
  - the retry CANNOT spin: prep returns 0 only when `jit_cow_clean()`
    holds. Clone-skipped-with-belt-intact degrades to correct-but-slow
    (the helper's own clone takes over, then fast resumes) - verified. A
    wrong value requires sabotaging BOTH the clone and the belt, and that
    combination HANGS (the retry spins) rather than corrupts - found by
    doing exactly that double-sabotage by accident.

MEASURED: 16_array_slice_write only -1.2%, and that is the honest number -
the bench takes a FRESH slice per iteration and writes ONCE, so its cost
IS the mandatory 1000-element clone, which prep relocates but cannot
remove. No suite bench has the shape prep serves (a write loop over an
array while a slice lives); the value is REACH - a slice no longer
permanently disables the fast path - proven functionally by the tier test
(fast-count + prep-count + value, per shape).

THE READONLY GUARD IS NOW PROVEN too (it was recorded as unproven): the
blocker was the specializer folding a const array into a write target's
base, fixed as its own bug (fold_lvalue_reads). With that fixed, the
readonly case reaches a compiled store through a plain param, and removing
the guard corrupts the const array - value-divergence caught.

The SLICE-BASE guard remains formally redundant with has_slices (a slice
registers ITSELF in its parent's set, so slice implies has_slices) - both
now route to prep, so the question dissolved: either check reaching prep
gives the same behaviour, decided in C++ by arr.is_slice().

## The first attempt, and why it was reverted

A full inline tier was written and is correct - guards + the raw store
emitted at the call site, the helper kept as the slow tier, every guard
declining to it so the negative wrap / OOB caret / COW / compound ops /
floats stay byte-identical. `-rt` was green in all four VM modes.

**It was reverted because it never FIRES**, and dead code that looks like
an optimization is worse than none. The counter proved it: a test that
asserts `g_jit_store_fast` moved reported "the INLINE path never ran" for
both the flat-int and the flat-bool shape.

## The root cause, which is the useful part

The guard `refcount == 1` is WRONG, and the reason is not obvious.

    var a = array(32);        ->   call.blt.v r4 = array(r3)
                                   move       a = r4

`MoveV` COPIES the handle, so the dead temp `r4` still holds one and
`use_count()` is 2 for the rest of the function. Every store then declines.
This is the shape of essentially all array code, so the tier was dead on
arrival.

The helper's own condition shows what the guard should be:

    if (arr.is_slice())            arr.clone_internal_vec();
    else if (arr.use_count() > 1)  arr.clone_aliased_slices(off + idx);

and `clone_aliased_slices` ITERATES `shobj->slices` - the set of live
slice VIEWS - so it is a **no-op when that set is empty**. A refcount > 1
with no slices is harmless: two variables sharing an array is MyLang's
reference semantics, and a plain store is the correct behaviour.

So the real condition is **"no live slice views"**, not "sole owner".

## What that needs, and the hazard

`shobj->slices` is a `std::set`; its size lives at a libstdc++-internal
offset, so it is not safely machine-checkable. It needs a mirrored
`bool has_slices` on `SharedObject`, maintained at the **9** insert/erase
sites in sharedarray.h (lines ~260, 274, 284-285, 299, 307, 315, 323-324).

**THE HAZARD, and why this was not rushed:** if one site is missed the
flag reads "no slices" while a slice exists, the inline store skips the
clone, and the live slice sees a value it should have been cloned away
from - a WRONG RESULT, silently, on a COW path. The mirror precedent in
this codebase (the VM's `rec_n` / size mirrors) always pairs the mirror
with an `ML_VM_CHECK` that re-verifies it; do the same here, in
`vm_store_elem_int_body`, so the interpreted path (which `-nbi` and the
JIT-off differential modes exercise on every store) validates the flag the
emitted path trusts.

## The plan, in order

1. Add `SharedObject::has_slices`, maintained at all 9 sites, with an
   ASSERTS-only `has_slices == !slices.empty()` check in the interpreted
   store body.
2. Extend `SharedArrayObj::jit_probe()` with `readonly`, `hash_valid` and
   `has_slices` (the co-located probe reads real members, so it cannot
   drift), plus an `LValue::jit_const_probe()` for the const flag.
3. Emit the tier: guards (array / not a slice / flat ints|bools / not
   const / not readonly / no live slices / index in [0,size)) then the raw
   store and `hash_valid = 0`; everything else declines to the helper.
   Plain stores only at first - compound needs read-modify-write.
4. **`rsi` IS RESERVED** - it carries the fragment's `t_int` singleton for
   the whole run. Clobbering it makes every later slot write stamp a
   garbage type; that was an immediate SEGV during this attempt. Use `rdi`
   for the value (free after `frag_entry` moves the slots base to `rbx`).
5. Prove it RUNS with a counter bumped by the EMITTED code
   (`g_jit_store_fast`, the `g_jit_boxed_fast` pattern) - the helper
   bumping `g_jit_op_run` cannot distinguish the tiers.

## #95 case 1 - COMPOUND stores (landed 2026-08-02)

`a[i] OP= v` (and the `a[i]++` lowering) joined the fast path as a
read-modify-write: same guards, hash invalidated first (frees the shobj
register - nothing can fault past the guards), then add/sub/imul or
cqo+idiv on the element; floats do `xmm1 = elem OP xmm0`.

The rules, each sabotage-verified:
  - EMIT-time refusals: float `%=` (fmod), literal 0/-1 divisors (the
    helper runs the exact interpreter C++ - the IntModRI convention).
  - RUNTIME divisor guards (slot rhs, div/mod): decline on 0 and -1,
    emitted BEFORE the prep jumps - div-by-zero throws WITHOUT cloning
    in the interpreter, so the clone side effect must not run first
    (prep=0 asserted with a live slice). OOB outranks div0 by
    construction: a decline re-derives everything in the helper's order.
  - the float zero test is ucomisd + a `jp` hop: unordered (NaN) sets
    ZF, so without the hop a NaN divisor silently declines (caught as
    fast-count 32 != 33).
  - the KIND checks moved BEFORE the prep jumps in every arm - prep
    must never clone a base the interpreter would fault on without
    cloning. Unreachable for this op's proven bases; the ordering is
    the invariant.
  - compound on runtime BOOL storage is COMPILE-unreachable (the
    compound int store pins the base to array<int>, so an array<bool>
    argument is a TypeMismatchEx); the ints-only kind guard is defense
    in depth. Probing for the shape exposed a PRE-EXISTING tw-vs-VM
    divergence (bool literal into an int-JOINED array), filed as its
    own bug.

A NEW SHAPE-EATER found here (now in CLAUDE.md's trap list): const-ARG
specialization + auto-const fold a param-derived write-once local
(`var z = n - n` under `f(9)`) into a LITERAL operand, so a "runtime
divisor" test exercises the emit-time refusal instead of the runtime
guard - three cases were vacuous exactly that way. Defeat: write-TWICE
locals (`var z = 1; z = n - n;`).

No suite bench compounds into an array element (62's `+=` is a DICT
store) - the value is reach + parity, like prep.

## #95 case 2 - the NESTED store tier (landed 2026-08-02)

`a[i][j] = v` / `OP= v` (StoreElem2V) gets the same treatment: the #93
outer navigation (general array, byte-length bounds, &row), then the
single-level store discipline on the row - const/readonly guards, KIND
and value-FIT and divisor guards all BEFORE the prep jumps (everything
the interpreter throws on without cloning), the shared prep on &row,
per-kind tails with the int RMW and the cvtsi2sd promote arm for an int
value into a float row. Chain stores (N-level) stay helper-only by
decision: data-driven walk, rare shape.

Sabotage-verified: row has_slices (slice oracle + prep), promote arm
(exact count), BOTH prep orderings (fit-before-cow, divisor-before-cow:
prep=0 fired each), divisor guards (ASan FPE). The OUTER-readonly guard
turned out SUBSUMED by the row's (deep const freezes every level, so a
readonly outer implies readonly rows) - kept as defense in depth,
honestly recorded as unprovable in isolation.

Building the float coverage EXPOSED a pre-existing no-fail-contract
bug: `row[j] = (j + 1) * 1.5` was REFUSED at compile time
(compile_float_expr had no arm for an int CHAIN subexpression, and a
proven-float flat store has no boxed fallback by design). Fixed - the
int subterm compiles as an int operand, which every float reader
promotes; pinned by a 5-mode differential entry.

## #95 cases 3 + 4 - promote + slice arms; THE MATRIX IS COMPLETE
## (landed 2026-08-02)

The nested read's INT row under LoadElem2Float promotes inline
(cvtsi2sd), and SLICE bases read inline at all three sites: the
single-level LoadElemInt/Float arm, and the nested read's OUTER-slice
and ROW-slice arms (the outer arm rejoins the common row section, so
slice-of-slices composes). A slice's elements live at data + (off + i)
and its bounds are the handle's u32 LEN, not the vector's size (probed
as arr_off_off/arr_len_off). Declines that stay on the helper: negative
indexes (the wrap), bool/other-kind slices, promote-under-slice.

Sabotage-verified: the promote arm (count 16 -> 0), the OFF addition in
both slice arms (parent-element value divergence - the 608 pattern),
and the LEN bound (a vector-size bound silently served sl[len]: count
17 and a missed OOB). The #56 slow-tier test's proof shape moved from
the slice to the negative wrap.

MEASURED (callgrind Ir, OPT=1 ASSERTS=0 both sides):
15_array_slice_readonly **-41.0%** (63.4M -> 37.4M); 14/43/46/18/01/16
all +-0.01%; the case-1/2 store restructure itself neutral-to-better
vs pre-#95.

The matrix's remaining DELIBERATE helper declines, each documented at
its emit site: float `%=` (fmod), nested float div/mod compound, chain
stores (N-level, data-driven walk), bool slices, promote-under-slice,
literal 0/-1 divisors. Dict load/store probes are a different arc.

## A trap in the TEST, worth avoiding next time

The first version of the decline cases counted every store in the whole
program, so a "must decline" case that also contained ordinary stores
reported a false positive ("served a shape it must REFUSE"). A decline
case must contain ONLY the decline shape, or the count has to be attributed
per site.
