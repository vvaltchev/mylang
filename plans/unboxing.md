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

   **THE EXACT EDIT SITES** (located 2026-08-01, so the next session does
   no rediscovery). A new opcode touches eight files; APPEND it to the
   enum so nothing renumbers, and bump the format version anyway, because
   an old binary would otherwise accept a new image and misread the op.

       src/bytecode.h    the OpCode enum (append, near LoadElemInt:191)
                         ML_FOR_EACH_OPCODE (append; line ~1153)
                         -- the enum/list order is static_asserted
       src/serialize.h   MYV_FORMAT_VERSION 9 -> 10   (line 26)
       src/codegen.cpp   the LOWERING - codegen.cpp:4268 already comments
                         that `a[i][j]` is base_array whose base is itself
                         a subscript; that is the site. Plus the tables at
                         242, 7031 (loc/caret extraction), 7386, 7462,
                         8000
       src/vm.cpp        the interpreted VM_CASE beside LoadElemInt:7570,
                         and the shared core beside the existing
                         vm_load_elem_int_core:2909
       src/jit.cpp       1028 (op classification), 2568
                         (pick_cached_slots - the base slots stay bad(),
                         the two INDICES are countable int uses), 3702
                         (the emit - extend emit_elem_base_gate /
                         emit_elem_int_read to two levels), 6531
       src/disasm.cpp    1024
       src/tests.cpp     15054 (the op-coverage switch) + the new tests

   **LANDED 2026-08-01, and BOTH kinds at once** - the int and float twins
   share every table entry and one emit case, so splitting them would have
   cost a second eight-file pass and a second format-version bump for
   nothing.

   Two things came out different from the sketch above:

   - **The JIT emit is a plain HELPER CALL**, not the predicted two
     inlined `emit_elem_base_gate` / `emit_elem_int_read` sequences. The
     win being bought is the deleted MATERIALISATION (a helper call plus
     a retain plus a 32-byte copy plus a slices registration plus a
     release), not the call itself, so the call tier captures it at a
     fraction of the emitter risk. An inline fast path stays available if
     a later measurement asks for it.
   - **The outer index must be a SLOT**, because `a`'s payload is spent
     on the DUAL (outer index, chain_locs idx). A literal outer index
     (`m[0][j]`) declines to the unfused pair. Materialising it into a
     temp was considered and rejected: it trades the fusion's win for an
     extra op on a shape that is not the target.

3. ~~**`LoadElem2Float`**~~ - done with the int one, see above.

4. **Decline paths.** A non-flat inner, a slice, a dict, a `dyn` base: fall
   to the existing `LoadElemValue` pair, byte-identical. The gate is
   compile-time (approach A - never a runtime bail into re-interpretation).

5. **Measure, then decide** whether a general/`dyn` inner deserves a slow
   tier, and whether B is worth opening.

## Expectations, and what actually happened

The prediction was **~221 -> ~90 Ir/iter, 27x -> ~11x**. The measurement,
callgrind Ir at `OPT=1 ASSERTS=0` on both sides, scale-1-vs-scale-3 delta
so the ~63M compile floor is excluded, n=70 so 343,000 inner iterations
per scale unit:

    base  75,962,673 Ir/scale   =  221.5 Ir/iter
    new   58,472,122 Ir/scale   =  170.5 Ir/iter     -23.0%

Whole-program -12.3% at scale 1, -17.9% at scale 3. Everything else is
neutral: 14_array_subscript +0.028%, 43_sieve +0.037%, 18_foreach_array
+0.023%, 01_while_loop +0.061%, 15_array_slice_readonly -0.546%. So the
new opcode costs no `vm_dispatch` layout tax.

**The prediction was wrong - 170.5, not 90, so ~21x rather than ~11x.**
What it got right was the ceiling; what it got wrong was assuming the
HELPER-CALL tier would reach it. Profiling the result says exactly where
the remaining 170 sits (scale 3, 1,029,000 inner iterations):

    jit_load_elem2_int (all inlined headers)  89.5M   =  87 Ir/iter
    EvalValue::operator=(&&)                  23.2M   =  22 Ir/iter
    the native fragment                       55.3M   =  54 Ir/iter

  - the **22** is `dst->put()` - the helper boxing an int result into the
    dst LValue. An INLINE emit would `store_dst(rax, target)`: two stores.
  - the **87** is two levels of `is<SharedArrayObj>` + `skind()` +
    `size()` + `offset()` + `get_vec()` + bounds check, none of which the
    emitted code can see through a call boundary.

So the INLINE tier - the two `emit_elem_base_gate` / `emit_elem_int_read`
sequences the original sketch called for, with the helper kept as the
slow tier for every declined shape - is worth roughly another 60-80
Ir/iter and would land near the original ~90 estimate. It is a
well-scoped follow-up now rather than a guess, because the split above
says which half of the cost it removes.

Two things the fusion does NOT change, both known going in:

  - The SUITE geomean does not move: the pair occurs in ONE program in
    bench/ + samples/, at three sites, one of them hot (step 1).
  - **It does not touch the other extremes.** 76_funcval_dispatch (23.8x),
    63_closures (23.7x), 11_closure_counter (21.8x) and
    10_recursion_deep (19.1x) are CALL PROTOCOL, not element access. They
    need the separate arc in plans/call-protocol-arc.md.

## One trap, recorded because it cost time

Proving the coverage test catches its bug (making the op JIT-INELIGIBLE
and confirming `jit_load_elem2_native` fails) also made `-rt` HANG, in
`opt_layer_equivalence` under `--no-opt all`, JIT-on only. The op was
then `op_fully_native` while NOT `jit_op_eligible` - a combination the
real tables never produce, and the shipped config passes that very test.
It was not root-caused. If a future op needs the same negative check,
flip BOTH predicates together, and treat a hang there as the artefact of
the inconsistent pair rather than a bug in the op.

## Testing

An emitter/op change, so: `-rt` on gcc-debug/ASan + clang-debug + hardened
release; the JIT-on vs JIT-off corpus differential over bench/ + samples/
(the tree-walker differential cannot see an emitter bug); `nested_fuzz.py`;
`-vdj` read by hand. Plus, specific to this op: an OOB on the OUTER and on
the INNER index must produce byte-identical carets to the unfused pair,
and a MUTATION of the outer array between iterations must behave
identically - write those before the op, and confirm they FAIL against a
deliberately wrong caret, per the "prove the test catches the bug" rule.
