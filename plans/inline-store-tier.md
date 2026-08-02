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

## What was built, and why it was REVERTED

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

## A trap in the TEST, worth avoiding next time

The first version of the decline cases counted every store in the whole
program, so a "must decline" case that also contained ordinary stores
reported a false positive ("served a shape it must REFUSE"). A decline
case must contain ONLY the decline shape, or the count has to be attributed
per site.
