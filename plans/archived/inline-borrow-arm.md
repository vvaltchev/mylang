# The emitted INLINE BORROW ARM (#94 step 3) — built, measured, REVERTED

**Status: REVERTED on 2026-08-15.** Correct, execution-proven, fully
tested — and worth zero wall clock while costing emitted bytes on
benches that never run it. This file is the complete record: why it was
built, what it measured, why it was reverted, and how to put it back
step by step if the core changes enough to make it pay.

**The tests it came with were KEPT.** Only the optimization was undone.

## The commits

| SHA | what |
|---|---|
| `c83bff2` | #93 the escape analysis (increment 1) |
| `aa3220f` | #93 transitive fixpoint + every rule pinned |
| `c063a6f` | correction: one recorded borrow hazard was not real |
| `a847379` | **#94 step 1** — `LValue::frame_release()`, one release point |
| `f6bbdb1` | **#94 step 2** — the borrow bind (helper tier). THE WIN |
| `36aa6ac` | step 2 measured: −13.5% Ir, 0.88x |
| `4bcb955` | the three hazard tests + the zeroing structural fix |
| `f6dfc9a` | #94 B — name the callback; `esc_collect` body walk |
| **`ddc2645`** | **#94 step 3 — THE INLINE ARM (this document)** |
| *(revert)* | reverts `ddc2645`, keeps the tests, keeps the movabs guard |

## What it was

Step 2 binds a non-escaping reference argument by calling
`jit_bind_ref_arg`, which decides at run time whether to borrow (a raw
bit-copy, no retain) or retain. Step 3 moved that decision into the
emitted push, so the borrow case calls nothing:

```
    mov  r11, [rsp]                  ; the desc spill (nothing pushed yet)
    mov  r11, [r11 + noescape_params]
    shr  r11, i                      ; omitted for argument 0
    test r11b, 1
    je   -> the helper               ; not claimed by the analysis
    movabs r10, <t_arr>              ; NOTE: movabs_r10, not movabs - below
    cmp  rax, r10
    jne  -> borrow                   ; a non-array reference is never a slice
    cmp  byte [rbx + s + slice_off], 0
    jne  -> the helper               ; a slice: must retain
  borrow:
    <the scalar arm's 32-byte copy>
    mov  byte [rdx + d + lv_borrow_off], 1
```

Both declines fall through to the existing helper, which re-asks both
questions — it is the C++ bind path's own tier and it owns the decline
counters, which is why the bit is loaded again there rather than passed.

## Why it was built

Step 2 left a `call` on a path bench 76 takes 1,000,000 times. Removing
a call that is pure overhead looked like free money.

## What it measured

Interleaved `--baseline`, `OPT=1 ASSERTS=0` both sides, `build/` deleted
first, both binaries under `build-claude/`.

| bench | instructions | wall clock |
|---|---|---|
| 76_funcval_dispatch (target) | **−3.27%** (−17 Ir/call) | **1.00x** |
| 10_recursion_deep (never borrows) | **+1.44%** | 1.05x |
| 63_closures (never borrows) | +0.48% | 1.07x |
| 26_dict_iterate | +0.01% | 1.14x (noise) |
| suite geomean | — | 1.003x |

Execution was PROVEN, not assumed: `g_jit_arg_borrow` was bumped by
generated code only, and bench 76 reported **999,999 of its 1,000,000
calls** served by the emitted arm with the helper at zero.

## Why it was reverted

The removed call was a **perfectly-predicted jump to an I-cache-resident
helper**, which retires nearly free beside the real work on a wide
out-of-order core. This is the exact finding already recorded for the
guard-elision family in `docs/jit-optimizations.md`, arrived at
independently a second time.

Against that zero benefit it charges a permanent cost: the arm is
emitted at **every reference-argument slot of every sync call site**,
whether or not the runtime value ever takes it. Two benches that never
execute a single borrow pay +1.44% and +0.48% instructions for the bytes
alone.

So: no gain where it runs, a real loss where it does not.

## When it WOULD pay — the condition to re-check before rebuilding

Rebuild it only if one of these becomes true, and verify with a
**wall-clock** A/B, never with instruction counts:

1. **The helper stops being hot.** The call is free today because
   `jit_bind_ref_arg` is called constantly and stays in L1i. A program
   mix where reference binds are sparse but hot-in-aggregate would pay
   the call for real.
2. **The emitted arm gets cheaper than ~40 bytes.** Most of the cost is
   code size at every call site. If the sync push ever gains a shared
   out-of-line per-callee stub, the arm could live there once instead of
   being inlined per site — that changes the trade completely.
3. **The bit becomes bakeable.** Today the callee at an emitted push is
   an inline CACHE, so `noescape_params` must be loaded at run time. If
   direct calls ever get a monomorphic emit path with the descriptor
   known at emit time, three of the arm's instructions vanish and only
   the slice test remains.
4. **A PMU becomes available.** This box is WSL2 with no performance
   counters, so the front-end/code-layout cost can only be inferred from
   wall clock. With real counters the trade could be measured instead of
   estimated.

## How to re-introduce it, step by step

The revert is a single commit, so the fastest route is
`git revert <the revert>` — but if the core has diverged, rebuild it in
this order. Each step is independently verifiable.

1. **`LValue::borrowed` must still exist** (step 2, `a847379` /
   `f6bbdb1`). If it does not, step 3 is meaningless — build steps 1-2
   first.
2. **Add the layout probe.** `LValue::jit_borrowed_probe()` returning
   `const bool &borrowed` (mirror `jit_const_probe`), a
   `lv_borrow_off` field in `JitLayout` (jit.cpp), and the wiring beside
   `l.lv_const_off`. Read the REAL member — a layout change must not be
   able to move the byte out from under the emitter.
3. **Add `desc_noescape` to `JitPushLayout`** (jit.h) and set it beside
   `L->desc_bind_req` in vm.cpp's push-layout init. (Step 2 already
   needs this for the helper's third argument; check before adding.)
4. **Add an emitted-only counter**, `g_jit_arg_borrow`, bumped ONLY
   inside the generated arm, reported by `MYLANG_JITSTATS` as
   `arg_borrow_nat`. Separate from `g_arg_borrow` (both bind paths)
   because the two tiers produce identical answers and nothing else can
   tell them apart.
5. **Factor the scalar copy** in `emit_sync_push_native` into a
   `raw_copy(bool borrowed)` lambda — the borrow IS the scalar copy plus
   one byte store, so sharing it is what keeps the two arms honest.
6. **Emit the arm** at the top of the reference branch, exactly as
   listed above.
7. **Update the counter assertions** in `jit_ref_arg_bind` and
   `jit_borrow_arg_shapes`: a case that previously asserted the helper
   ran must assert `g_jit_ref_arg_binds + g_jit_arg_borrow`, since the
   inline arm now serves the borrowing shapes. Add one assertion on
   `g_jit_arg_borrow` alone as the execution proof.
8. **Sabotage all three decisions** and watch each fail: force the
   noescape bit set (caught by `borrow_from`'s ML_CHECK), force "not a
   slice" (caught by the #94 test on a VALUE), drop the `borrowed` byte
   store (caught by ASan).
9. **Measure wall clock, interleaved.** If it is not a wall-clock win,
   it is not a win — that is the whole lesson of this file.

## ⛔ Two emitter traps this cost real time on

Both assemble silently into a *different instruction*, and both are now
guarded in the source:

- **`Emitter::movabs` is LOW-EIGHT-REGISTERS ONLY.** Its REX is a bare
  `0x48` with no REX.B, so `movabs(R10, x)` emits `0xC2` — `ret imm16` —
  and the fragment returns into nothing. Use `movabs_r8` / `movabs_r10`.
  `movabs` now `ML_CHECK`s `reg < 8`; **that guard was KEPT by the
  revert** and is the one piece of step 3 still in the tree.
- **`Emitter::j32` takes the SHORT jcc opcode** and adds 0x10 for the
  near form, so `je` is `j32(0x74)`. Passing the near second byte
  (`0x84`) assembles `0F 94`, a SETE with a bogus modrm. Documented at
  `movabs` beside its sibling.

## The structural fix from `4bcb955` that must NOT be undone

Step 3 sits on top of it, but it stands alone and stays: the emitted
push's slot zeroing lives **inside the scalar arm**, not in a loop after
all the binds. The qword at +40 covers `container_idx` and BOTH flag
bytes, so a trailing tidy-up store wipes `borrowed` and the frame pop
then releases a count the slot never took — a use-after-free from a
store that reads as housekeeping. Watched failing: ASan.
