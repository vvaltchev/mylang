# The top-5 my/cpp gap, decomposed (2026-08-12)

Post-flip suite (OPT=1 ASSERTS=0, norec default, fresh-stamped cpp
cache): geomean my/cpp **2.514x** over 77 pairs. The top-5, all
call/closure protocol:

    76_funcval_dispatch   18.66x   836 Ir/iter (1 value call)
    11_closure_counter    18.59x   355 Ir/iter (1 closure call)
    78_typed_param_call   15.66x   889 Ir/iter (2 closure calls)
    63_closures           14.19x   426 Ir/unit (lifecycle mix)
    75_indexed_unpack     13.32x  1976 Ir/unit (unpack + strings)

FAIRNESS AUDIT FIRST (the 78 lesson): 76 keeps one true `call *%rax`
per iteration, 11/63 call through std::function, 75 was class-E
audited - the four twins are fair; 78 was the only ceiling and is now
std::function too (fair race measured 17x where the ceiling said 71x).

## Why std::function is ~10 instructions and MyLang ~250+

The COMPLETE std::function call in the fair twin: a null check, two
arg moves, `call *0x38(%rsp)`, then the invoker - `endbr64; mov
(%rsi),%rax; add (%rdi),%rax; ret`. Nine to eleven instructions, all
in. It pays NO stack accounting (one rsp it already has), NO type tags
(static types), NO refcounts (the capture is a POD long), NO parallel
frame view (DWARF unwinds cost zero until thrown), NO dispatch guard
(the signature pins the target set at compile time).

MyLang's record-less call still pays, per call (78's fragments ≈250
Ir/call + helpers): the callee-value load + identity guard; the
window allocation with seg-top/used accounting + the high-water gate;
per-arg binds (type store + value store, ref-aware); the vframe
repoint (the C++-helper-visible view); the captures swap + residue;
the callee prologue (rbp chain + callee-saved); and the return arm
(window compare, ref-aware dst write, accounting restore, captures
restore). Each piece is a real obligation of dynamic typing +
refcounting + walkable frames - but two of the top-5's biggest costs
are NOT protocol at all:

## The per-bench decomposition (callgrind s3/3, perf build)

**78 (889/iter, 2 calls):** emitted ≈503; **boxed arithmetic ≈274**
(num_bin_op 92 + jit_boxed_binop 126 + vm_num_binop 56); EvalValue
moves 50. The closure bodies `base + k` / `f * x` run BOXED: a
CAPTURE operand defeats the typed lowering (verified: the lambda body
disassembles to one boxed `+` in boxed_ops), even though inference
types it and the TREE-WALKER has eval_int capture reads. ~137 Ir per
call where C++ pays 1 add.

**11 (355/iter):** emitted ≈193; **the `start++` capture compound
store ≈105** (jit_store_capture_compound 75 + vm_num_binop 30);
EvalValue moves 50. Same root cause, store side.

**76 (836/iter):** emitted ≈299; **the `ops[i%2]` func-value
materialization ≈209** (TypeArr::subscript 109 + jit_subscript 64 +
copy_assign 36) plus release/bind/move ≈107. Reading one func element
into a temp costs ~200 Ir of boxed copy + refcount before the call
guard even runs.

**63 (426/unit):** closure creation (FuncObject ctor ≈30 +
jit_make_closure 10) + jit_ret_norec 28 (ref results take the C++
decline tier by design) + capture stores 30 + moves 44.

**75 (1976/unit):** vm_unpack_elem_body 435 + arr_elem_at 400 +
LValue::put 250 + SharedStr move/copy ≈240. Each unpacked element is
read into a boxed temp then put into the slot - two moves + string
refcount churn per element.

## The reduction map (sized, unordered - the maintainer sequences)

- **H1a - TYPED CAPTURE READS: DONE (2026-08-12).** No new opcode was
  needed: `try_capture_leaf` materializes the capture with the existing
  boxed LoadCaptureV into a temp, and the temp IS an int/float frame
  slot the typed ops read by tag. Measured (interleaved --baseline,
  full suite): **78 = 0.75x wall, -23.7% Ir/iter, my/cpp 15.6x -> 11x**;
  geomean cur/base 0.999x; 67_make_dict's callback additionally became
  a native_leaf. 11 and 63 are byte-identical - their cost is the
  capture STORE, which is H1b. Full record: docs/jit-optimizations.md.
- **H1b - TYPED CAPTURE WRITES (11 + 63), NEXT.** `count++` still
  lowers to a compound StoreCaptureV (copy-modify-store through
  num_bin_op: ~105 of 11's 355 Ir/iter). With H1a's typed READ in
  hand the shape is `LoadCaptureV t; IntBin t2 = t + 1; <typed store>`
  - i.e. only a PLAIN typed capture store is missing. **Gate it to the
  compound / inc-dec forms**: a plain `cap = <rhs>` whose rhs is
  `th == i` may be a BOOL value, and storing it as an int would change
  the observable type (the #96 hazard); a compound's result is int by
  arithmetic promotion, so that question does not arise.
- **H1 (original entry) - TYPED CAPTURES (the biggest, hits 78+11+63).** Give the
  VM/JIT typed capture-slot reads/writes: the inferencer already
  types the captured var (by-value snapshot), CaptureSlots' data
  pointer is already a bare `mov` (G2's layout contract), and the
  tree-walker already reads captures in eval_int - only the codegen
  declines. A capture operand with proven int/float lowers into the
  typed ops (a capture-read source / capture-write dst variant, or a
  typed load-to-temp), `start++` becomes load/inc/store behind the
  existing type proof. Projected: 78 −30%, 11 −30%, 63 −10-15%.
- **H2 - FUNC-VALUE ELEMENT CALL (76).** The `ops[i%2]` boxed
  materialization (~200 Ir) borrows instead: an elem-read callee
  feeding CallValueV can be guarded + called off a borrowed
  `const EvalValue &` (the local `ops` holds the array across the
  call), LoadElem2-style. Projected: 76 −20-25%.
- **H3 - SINGLE-MOVE UNPACK (75).** arr_elem_at straight into the
  dst slot with one retain, no boxed temp round-trip. Projected:
  75 −25-35%.
- **H4 - the structural residue (the G1 arc's next act): window on
  the native stack** - kill the seg-top/used accounting + restore
  (~30-40 Ir/call). Big design step (StackOverflowEx, the walker,
  helper visibility); not sized here.

Honest ceiling: with H1-H3 landed these benches project to ~8-11x -
the remaining gap is the value model itself (48-byte LValues, type
tags, refcounts on every func/str value), i.e. #60's territory, not
the call protocol's.
