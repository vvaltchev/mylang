# The JIT loses a call site's virtual backtrace frames (task #88)

## Status

Two of the three defects in this area are FIXED and pinned - the
loc-less throw at its origin in `9fe3783` (#76), the dropped call-site
frames in `c1310ed`, both by one two-depth parity test -
both were interpreted-path bugs and both reproduce with `-nj`:

1. a folder-synthesized divisor carried no `Loc`, so the VM threw with no
   location at all (fixed in `extract_locs`);
2. the four call-site flushes went through the once-guarded raise-site
   helper, dropping every virtual frame after the first (fixed by
   `vm_flush_inline_call`).

**What remains is JIT-only.** With the JIT on, a recursion whose self-call
sits inside an inlined region still renders fewer frames than the
tree-walker, and the innermost frame can be misnamed.

## The repro

```
func f(n) {
    if (n < 2) return 1 / (n - n);
    return f(n - 1) + f(n - 2);
}
print(f(4));
```

`-tw` and `-nj` agree exactly. The JIT drops the two virtual frames that
follow the physical one and attributes `main()` to line 3 instead of line 5.
At depth 3 it additionally renders the innermost frame as `f$0` (a virtual
frame that should not be there) where both other engines render `f`.

## The single root cause

**Once a run's interpreted originals are deleted, the pc-keyed side tables
are degenerate.** `-vd` on this program shows `f$0`'s chunk collapsed to 9
`enter.nat` ops - and **54 loc entries, most of them at `pc0`**. So
`chunk.loc_at(pc)` and `chunk.inline_frame_at(pc)` return *the first of 54*.
Any JIT path that resolves a side table by pc is reading a coin flip.

That is precisely why `emit_exc_stamp` bakes the caret and the chain
(`Exception::jit_inline_frame`) at emit time, from the OLD pc, before the
remap. The baking covers exactly one case - **the RAISE site, when the op
has a chain** - and the two remaining defects are the two holes in it:

- **A raise whose op has NO chain does not bake anything**, so
  `vm_flush_inline_walk` falls back to `inline_frame_at(pc)` and invents a
  chain from a collapsed pc. `-1` means both "not stamped" and "stamped: no
  chain", and the consumer cannot tell them apart. (This is the depth-3
  symptom.)
- **A CALL SITE's chain is never baked at all.** `emit_exc_stamp` is
  first-conveyor-wins, so by the time an exception reaches an enclosing
  call site the field already holds the innermost raise's chain and cannot
  be reused. (This is the missing-frames symptom.)

A pc lookup at `EnterNative` was tried and **rejected**: it recovered one
frame on the depth-4 shape purely by luck and broke depth 3, because it is
the same degenerate lookup.

## The fix

Bake both missing cases, mirroring the caret's existing treatment.

1. **Disambiguate the raise-side sentinel.** `emit_exc_stamp` currently
   early-returns when the op has neither a loc nor a chain; make it stamp
   `-2` ("known: this op is not inlined code") so `vm_flush_inline_walk`
   skips the pc fallback instead of inventing a chain. The first-wins guard
   becomes "field != -1" rather than "field >= 0".

2. **Bake the call site's chain.** Add `Exception::jit_call_inline_frame`
   (default -1) plus its `jit_off_*` accessor, stamped at the sync call's
   conveying exits and consumed - and CLEARED - where the frame is already
   handled.

   The natural home for the flush is the conveyance tail that already
   exists in **both** `jit_sync_postexit` (vm.cpp ~6146) and
   `jit_call_sync_core` (~6466): each pops the callee frame and stamps its
   baked `site_packed` onto it, which is exactly `do_func_call`'s
   "capture the frame, then flush the call site's chain" shape - the flush
   belongs on the next line. Both helpers already take `site_packed`, so
   this is one more baked argument.

   **Do NOT bake a `Chunk *`** - `codegen_chunk` builds the chunk on the
   stack and moves it out after `jit_compile_chunk`, so the address
   dangles. Bake `ck.inline_frames.data()` instead: a vector's heap buffer
   survives the move, which is the same argument that makes the existing
   `&ck.locs[i]` bake safe.

## Testing

`inlined_recursion_backtrace_parity` (tests.cpp) already runs the two
shapes at depths 2 and 4 and compares the engines byte-for-byte; it forces
`g_jit_enabled = false` for now. **Removing that line is the acceptance
test** - when the fix lands, the same test must pass with the JIT on.

Keep the two-depth structure: the unroll produces a different shape at
each, and a single-depth version of this test passed with defect 1 put
back.
