# Unboxing: stop materialising intermediate containers

Status: **SCOPED, not started.** Written 2026-08-01 after measuring why
46_matrix_mult is 23x C++.

## The measurement that motivates it

`s += a[i][k] * b[k][j]`, instructions per INNER ITERATION (scale-1 vs
scale-3 delta, so compile time is excluded; `OPT=1 ASSERTS=0`):

    MyLang 221.5      C++ 8.2      27x

Where MyLang's 221 go (callgrind at scale 10, so the ~63M compile floor is
8% and does not distort):

    JIT fragment (real native code)          76.7   35%
    jit_load_elem_value (a HELPER call)      72.0   33%
    TypeImpl<SharedArrayObj>::copy_assign    39.4   18%
    LValue::put                              23.0   10%

**Dispatch is not the problem - it is already gone.** Only a third is
native fragment code, and much of that is marshalling for the helper.
**61% is inside C++ helpers the JIT calls.**

LICM already hoists `a[i]` out of the k-loop, so what remains is
essentially **`b[k]` alone - fetching one array element that happens to
itself be an array - at ~134 instructions.** C++ gets the row with one
`mov`.

## What the bytecode actually does

    3  load.elem.v  r4 = b[k]      <- the ROW, boxed into a temp
    4  load.elem.i  r5 = r4[0]     <- one int read out of it

`r4` is a full 48-byte `LValue` holding an `intrusive_ptr<SharedObject>`.
Materialising it costs a helper call, a refcount retain, a 32-byte copy,
`SharedArrayObjTempl`'s copy ctor registering it in the parent's live-
slices set, and then a release when the temp is overwritten next
iteration. **It is alive for exactly one instruction.**

## Two ways to fix it

### A. Fuse the nested READ (recommended first)

Never emit the pair. One op walks both levels and reads the scalar, so the
row is BORROWED - a raw `SharedObject *` that never leaves the op.

**The soundness argument is short, which is the point:**

  - the borrow is read and consumed inside ONE instruction; no user code,
    no allocation, no mutation, no unwinding can run in between;
  - the OUTER array holds a reference to the row for the whole op, so the
    row cannot be freed under us;
  - the op only READS, so it can trigger no copy-on-write detach;
  - the live-slices registration exists so that a write to a base can
    detach live VIEWS - we create no view, so there is nothing to
    register.

There is no lifetime question to answer because no reference outlives the
instruction. That is what makes A tractable where B is not.

**The asymmetry that says this is a gap, not a design choice:** the STORE
side already does exactly this. `StoreElem2V` walks `a[i][j] = v` in one
op, and `StoreElemChainV` generalises it to any depth, both with per-step
carets from the `chain_locs` pool. The READ side just never got the twin.

It is also the same shape as three things already shipped: the #9 fusions
(`JumpUnlessElemInt`, `ForStepElemInt`), lever 4b's `OrdCharV`, and the
struct-array `LoadStructFieldInt` - each removes an intermediate that only
existed to be consumed immediately.

### B. Borrowed references in the value model

Let a frame slot hold a non-owning reference generally, so any chain of
container accesses avoids boxing. This is the real "unboxing" and it is
much larger: it touches copy-on-write (when does a detach invalidate a
live borrow?), slices, the `ref_slots` release scan, and every builtin
that takes an `LValue *`. The failure mode is a dangling pointer, i.e.
silent memory corruption.

**Do not start B until A has been measured.** A tells us how much of the
27x is this shape at all; if A gets 46_matrix_mult most of the way, B's
remaining case is narrower and can be scoped against real numbers instead
of a guess.

## Increments for A

1. **Instrument the reach FIRST.** DONE, and the answer is small:

       3  bench/my/46_matrix_mult.my
       total fusable sites: 3

   Across ALL of bench/ + samples/, the `load.elem.v` -> `load.elem.*`
   pair occurs in ONE program, at three sites, and only ONE of the three
   is in a hot loop (the other two are the checksum reads, executed twice
   per scale iteration). So the fusion would fire on exactly one hot site
   in the whole corpus.

   **Read that carefully before concluding it is not worth doing.** bench/
   is a construct-by-construct suite: it has one array-of-arrays program
   because someone wrote one, not because nested containers are rare in
   real code - grids, boards, images, tables and adjacency lists are
   ordinary MyLang shapes. The number measures the SUITE's coverage, not
   the language's usage. And the STORE side (StoreElem2V) was judged worth
   building on reach that cannot have been better.

   What it does settle: the SUITE geomean will not move, so do not expect
   one, and do not justify the work with it.

2. **`LoadElem2Int`** - `dst = base[i][j]`, base proven
   `array<array<int>>`, both indices int-compilable, both storages flat.
   Interpreted body first (a shared `vm_load_elem2_int_core`, so the JIT
   and the VM cannot drift - the standing rule), then the JIT emit.
   Carets: the OUTER index's OOB must report the outer subscript's span
   and the inner's the inner - which is precisely what `chain_locs`
   already stores for the store side, so reuse the pool rather than
   inventing a second one.

3. **`LoadElem2Float`** - the twin, once the int one is green and
   measured.

4. **Decline paths.** A non-flat inner, a slice, a dict, a `dyn` base: fall
   to the existing `LoadElemValue` pair, byte-identical. The gate is
   compile-time (approach A - never a runtime bail into re-interpretation).

5. **Measure, then decide** whether a general/`dyn` inner deserves a slow
   tier, and whether B is worth opening.

## Honest expectations

  - 46_matrix_mult: ~221 -> ~90 Ir/iter, so **27x -> ~11x**. That is the
    one bench this targets squarely.
  - The SUITE geomean will barely move. Very few programs in bench/ +
    samples/ do nested container reads in a hot loop - step 1 exists to
    find out exactly how few before any code is written.
  - **It does NOT touch the other extremes.** 76_funcval_dispatch (23.8x),
    63_closures (23.7x), 11_closure_counter (21.8x) and
    10_recursion_deep (19.1x) are CALL PROTOCOL, not element access. They
    need the separate arc recorded in plans/cpp-gap-extremes.md.

## Testing

An emitter/op change, so: `-rt` on gcc-debug/ASan + clang-debug + hardened
release; the JIT-on vs JIT-off corpus differential over bench/ + samples/
(the tree-walker differential cannot see an emitter bug); `nested_fuzz.py`;
`-vdj` read by hand. Plus, specific to this op: an OOB on the OUTER and on
the INNER index must produce byte-identical carets to the unfused pair,
and a MUTATION of the outer array between iterations must behave
identically - write those before the op, and confirm they FAIL against a
deliberately wrong caret, per the "prove the test catches the bug" rule.
