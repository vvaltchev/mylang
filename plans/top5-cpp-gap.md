# The top-5 my/cpp gap, decomposed (2026-08-12)

Post-flip suite (OPT=1 ASSERTS=0, norec default, fresh-stamped cpp
cache): geomean my/cpp **~2.47x** over 77 pairs (the 2.514x first
recorded here was measured on `build/mylang` - run.py's DEFAULT
--mylang, i.e. the MAINTAINER's binary, not the lane under test. Pass
`--mylang build-claude/<lane>/mylang` explicitly; the per-bench
RANKING below is unaffected, and the callgrind decomposition was
always taken on build-claude/perf). The top-5, all call/closure
protocol:

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
- **H1b - TYPED CAPTURE WRITES: DONE (2026-08-12).** `cap++`/`cap OP= v`
  recomposes into materialize + IntBin/FloatBin + a PLAIN store (which
  has a JIT inline tier; the compound never did). `+ - *` only - those
  cannot throw for proven operands, so there is no caret to preserve;
  `/` and `%` were MEASURED moving the div0 caret two lines and are
  excluded, pinned by an `err loc:` test. Measured (H1a+H1b, interleaved
  full suite): **11_closure_counter 0.55x** (18.6x -> 9.08x my/cpp),
  78 0.80x, 63 0.90x; suite geomean cur/base 0.986x, my/cpp **2.424x**.
  The ORIGINAL H1b sketch below is superseded.
- **H2 - FUNC-VALUE ELEMENT READ: DONE (2026-08-12), and the
  PROJECTION WAS WRONG.** The borrow idea in the original sketch is
  UNSOUND and was dropped on inspection: unlike LoadElem2's row (used
  and discarded inside one instruction), the callee must stay alive
  ACROSS the call, and the callee can mutate `ops` - so the retain is
  load-bearing, not waste. What shipped instead: the read lowers to
  LoadElemValue (universal over storage kinds now) rather than the
  generic SubscriptV, deleting a helper frame, a Type virtual and the
  LValue back-pointer work that jit_subscript RValue()s away. Measured
  **-12.2% Ir on 76 (836 -> 734 per iteration) with the WALL CLOCK
  FLAT** (1.01x; the suite's byte-identical benches swing 0.89-1.13x in
  the same run). The removed work is cheap predicted L1 work that
  retires alongside the memory-bound call protocol - the documented
  instruction-vs-time divergence. Two audit-table gaps were fixed on
  the way (LoadElemValue was a liveness BARRIER and un-retargetable).
  **76's remaining 734 Ir/iteration is the CALL PROTOCOL and the two
  arg copies - not the element read**, so the next move on 76 is H4
  (the window/accounting residue), not another read tier.
- **H3 - SINGLE-MOVE UNPACK (75). TARGET PROVEN WALL-CLOCK-VISIBLE
  (2026-08-12), implementation open.** After H2 the first question is
  no longer "how many instructions" but "does the time move". For 75
  it does. A four-way probe at scale 4, best-of-5, same binary:

        bare counter loop, no bind            0.01s
        foreach + ONE array-element bind      0.16s
        foreach + 2-STRING unpack, unused     0.27s
        the bench (unpack + 2 len + arith)    0.28s

  So **the element BINDS are ~93% of this bench's wall time** (0.26 of
  0.28) and the loop machinery is ~4%; the `len()` calls and the
  arithmetic together cost 0.01s. Per element that is ~6.7ns (~20
  cycles) for what is logically a 24-byte handle copy plus two
  refcount RMWs. (Note a control: replacing the unpack with
  `row[0]`/`row[1]` subscripts is SLOWER - 0.44s - so the unpack op is
  already the better of the two lowerings; the cost is inside the
  BIND, not in choosing it.)

  WHERE IT GOES, per element (vm_unpack_elem_body's value branch):
  `vm_arr_elem` builds `EvalValue(SharedStr(flat_strs()[at]))` - one
  intrusive_ptr RETAIN - and `LValue::put(EvalValue&&)` then runs
  `EvalValue::operator=(EvalValue&&)`, whose work for a `t_str` value
  is an INDIRECT call through `type->move_assign` (plus destroy/create
  through two more function pointers whenever the slot's current type
  differs). The type erasure is the cost: the types are statically
  known at that point (a `strs` sub-array yields a SharedStr) but the
  value model reaches them through pointers.

  THE SHAPE OF THE FIX: in the `strs` branch, when the destination is
  a plain frame slot ALREADY holding a SharedStr (the steady state
  after the first iteration), assign the handle directly -
  `dst.getval<SharedStr>() = SharedStr(src)` - which is an
  intrusive_ptr release+retain with NO type-erased dispatch; anything
  else (a const slot, a container-backed slot, a different current
  type) keeps `put()`. NOTE `SharedStr`'s DELETED ctor is the one from
  `std::string`, not the copy ctor - copying a handle is allowed and
  is exactly a retain. Sibling cases to enumerate before calling it
  done: the general-storage branch (a func/array/dict element - the
  same indirect-call chain), the flat int/float branches (already
  direct via write_*_slot), and MultiUnpackV, which shares the value
  model but not this function.
- **H4 - the structural residue (the G1 arc's next act): window on
  the native stack** - kill the seg-top/used accounting + restore
  (~30-40 Ir/call). Big design step (StackOverflowEx, the walker,
  helper visibility); not sized here.

## Where the top-5 stands after H1 (measured, same interleaved run)

    76_funcval_dispatch   18.83x   (unchanged - H2 is its item)
    75_indexed_unpack     13.72x   (unchanged - H3 is its item)
    78_typed_param_call   11.54x   (was 15.66x)
    63_closures           11.44x   (was 14.19x)
    11_closure_counter     9.08x   (was 18.59x - out of the top five)

Suite my/cpp geomean **2.424x**. Honest ceiling: with H2 + H3 landed
these project to ~8-11x - past that the gap is the value model itself
(48-byte LValues, type tags, refcounts on every func/str value), i.e.
#60's territory, not the call protocol's.
