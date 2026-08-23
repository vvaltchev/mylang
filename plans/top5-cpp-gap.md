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
- **H3 - SINGLE-MOVE UNPACK (75): DONE (2026-08-12).** Measured
  (interleaved --baseline, full suite): **75_indexed_unpack 0.84x wall,
  -25.4% Ir** (1991M -> 1486M at scale 1, callgrind - ~50 Ir per
  element bind over 10M binds); suite geomean cur/base 1.002x, with the
  same run's untouched benches swinging 0.91-1.13x (73's 1.11x and
  20's 1.05x sit inside that band beside untouched movers on both
  sides; neither touches the changed arms - 20 is the flat-int unpack,
  73 is int elements through the unchanged default arm). What shipped:
  `vm_slot_bind_str`/`vm_slot_bind_value` (vm.cpp) - when BOTH sides
  are strings and the slot is plain (no container back-pointer), the
  bind is a direct SharedStr copy-assign (intrusive_ptr release+retain,
  self-assign-guarded, fully inline, ZERO indirect calls); a
  general-storage non-string element improves to put(const &) in place
  (one copy_assign instead of copy_ctor + move_assign + temp dtor).
  Wired into vm_unpack_elem_body (both loops; jit_unpack_elem funnels
  through it) and vm_multi_unpack_body's plain stores; compound +
  numeric-coerce arms untouched. Proof: g_unpack_fast_binds bumps ONLY
  in the dispatch-free arm - unpack_fast_bind_shapes asserts growth per
  shape and EXACTLY 0 on alternating str/int re-binds; both sabotages
  watched failing (fast arms disabled -> the counter check fails;
  a one-short window from the fast arm -> the value tests + the
  pre-existing unpack tests fail). REMAINING SIBLINGS (enumerated, not
  built): the single-var foreach VALUE bind (`foreach s in rows` -
  do_iter / the foreach Next op pays the same put() chain; the probe's
  one-bind loop cost 0.16s of 75's 0.28s shape, so it is the same class
  of target), and the tree-walker's bind_loop_var (perf parity only -
  values already identical). The ORIGINAL probe record follows.
  After H2 the first question is
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

# RE-MEASURED AFTER H1+H2+H3 (2026-08-12): the twins are FAIR, the gap is ours

Suite my/cpp geomean **2.361x** (was 2.47x pre-H1). The top five:

    76_funcval_dispatch   17.60x   734 Ir/iter
    78_typed_param_call   10.87x   678 Ir/iter
    75_indexed_unpack     10.63x   294 Ir/row
    63_closures           10.46x
    11_closure_counter     9.11x

## The ratios are UNDERSTATED, not inflated (fixed-cost audit)

These C++ twins run 1-10 ms total, so process startup is a real fraction
of them - which SHRINKS the printed ratio rather than inflating it.
Measured floors: an empty C++ binary **0.3 ms**, a trivial MyLang script
**1.3 ms**. Re-measured as a SCALE DELTA (time at scale 2 and 10,
subtract - removes process startup, MyLang's compile AND the JIT warmup
from both sides, best-of-9):

    bench                   my/unit    cpp/unit   ratio   (printed)
    76_funcval_dispatch     0.03027    0.00122    24.73x   (17.60x)
    78_typed_param_call     0.02446    0.00214    11.43x   (10.87x)
    75_indexed_unpack       0.06089    0.00511    11.93x   (10.63x)
    63_closures             0.01388    0.00069    20.14x   (10.46x)
    11_closure_counter      0.00897    0.00065    13.77x    (9.11x)

So the honest compute gap is WORSE than the table says (76 is ~25x, 63
~20x). Nothing here is a measurement artifact.

## FAIRNESS AUDIT (asm-verified, not asserted)

- **76**: the C++ loop keeps a real `call *%rax` (the target reloaded
  from `ops[i&1]` every iteration - no devirtualization, no unswitch);
  8 instructions + the call.
- **78**: `cmpq $0,48(%rsp); je; call *56(%rsp)` - a genuine
  std::function indirect call WITH its null check, both closures, not
  inlined. (This is the already-fixed twin; the fix holds.)
- **75**: models what MyLang actually does - a volatile refcount ++/--
  per handle bind and the STRICT arity check per row.
All three are fair. **The gap is MyLang's codegen, not the twins.**

## WHERE THE INSTRUCTIONS GO (callgrind, per unit of work, perf build)

**76 - 734 Ir/iteration** (C++ ~8):

    jit_load_elem_value  (read ops[i%2])       1 call    171 Ir
    the `st` reference ARG traffic:                      184 Ir
       jit_bind_ref_arg                        1 call     78
       SharedArrayObj::copy_assign             2 calls    74
       SharedArrayObj::dtor                    2 calls    22
       SharedArrayObj::default_ctor            2 calls    10
    the two callee fragments (the REAL work)   1 call     80 Ir
    jit_move                                   1 call     81 Ir
    jit_release_slot                           1 call     54 Ir
    jit_put_int  (st[0] = ...)                 1 call     50 Ir

**78 - 678 Ir/iteration**: `jit_boxed_binop` **190 Ir** -> vm_num_binop
90 -> num_bin_op 61 -> TypeFloat::add 15. ONE float addition, 190
instructions. The two closure fragments are 149.

**75 - 294 Ir/row**: `jit_unpack_elem` 239 -> vm_unpack_elem_body 217
-> vm_unpack_bind_elem 122 (2 calls). Still a 4-deep helper chain per
row even after H3 (which cut ~100 Ir off it).

## THE THREE ROOT CAUSES (each verified, each with a fix shape)

### H4 - A CALL RESULT IS NOT A TYPED OPERAND (the biggest, and the
### cleanest: it is the H1a shape again, one level up)

The whole typed expression degrades to the boxed tier when ANY operand
is a call. Controlled A/B, same statically-int accumulate:

    var q = runtime(3); s = s + q;      ->  i.bin  r2 = r1 + r0   UNBOXED
    var f = mk(7);      s = s + f(i);   ->  bin.v  s = s + r4     BOXED

Nothing is unproven here - `-dti` says `add : func(int)->int`,
`scale_it : func(float)->float`, `s : int`, `t : float`, and M8 DID
build `TypedScalarExpr<arith,i>(CallExpr(...))` /
`TypedScalarExpr<arith,f>(CallExpr(...))`. The **codegen** refuses a
call as a typed leaf and falls back for the entire node. That is
exactly what a CAPTURE operand did until H1a, and the fix is the same
one: materialize the result into a temp with the EXISTING call op, then
read the temp as a typed leaf (`try_call_leaf`, beside
`try_capture_leaf`), with a result type-tag guard for soundness so the
tier never depends on inference being right.
REACH: 78 (both accumulates), and every call-heavy bench - 12, 35, 63,
11. On 78 it removes the 190 Ir float chain outright and the int guard
tier with it.

### H5 - THE VALUE-MODEL ASSIGN COSTS 37-100 Ir WHERE ~10 IS THE WORK

`EvalValue::operator=` reaches a non-trivial type through THREE
function pointers - `dtor` -> `default_ctor` -> `copy_assign` - on any
type change. For a SharedArrayObj that is 106 Ir/iteration on 76 to
copy one handle twice, when the real work (an intrusive_ptr
release+retain plus three POD fields, both slice tests false) is ~10
inline instructions. The same triple sits behind `jit_bind_ref_arg`
(78), `jit_load_elem_value` (171) and every return.
**H3 is the proof of concept**: it deleted exactly this triple for
STRINGS in the unpack ops and took 75 to 0.84x wall / -25.4% Ir. The
general form is a statically-typed assign path - when the emitter knows
both sides' types (it usually does), call a monomorphic assign or emit
it inline, instead of the type-erased triple-dispatch. This is #60's
core thesis and it is where 76's remaining gap lives.

### H6 - THE BOXED INLINE FAST TIER IS INT-ONLY

jit.cpp's own comment: *"ANY other shape (float/bool/string/mixed
operand, a throwing aop) falls to the EXACT helper path below"* - the
guard compares against `t_int` and there is no float twin. That is why
78's float accumulate pays 190 Ir where the int one pays inline guards.
A classic sibling-case gap (the CLAUDE.md rule). LOWER priority than
H4, which removes the boxing altogether for the PROVEN cases; H6 only
catches the genuinely-`dyn` residue (66_dyn_foreach's float sibling).

### H7 - the unpack is still a 4-deep helper chain (75)

Post-H3 it is 239 Ir/row through jit_unpack_elem -> vm_unpack_elem_body
-> vm_unpack_bind_elem x2. The element STORE (#92) and the nested READ
(#93) both got emitted INLINE tiers for the same reason; the unpack
never did. Sized ~150-200 Ir/row.

## RECOMMENDED SEQUENCE (the maintainer picks)

1. **H4** - biggest reach, smallest change, precedent already written
   (H1a). Unblocks typed arithmetic across every call-heavy program.
2. **H5** - the deepest win (it is what makes 76 a 25x bench) but the
   largest design step: it touches the value model's hot path
   everywhere. H3 proved the shape on one op; generalizing needs care.
3. **H7** then **H6** - both are bounded, local, sibling-case work.

HONEST CEILING: H4+H5 plausibly take 76 from ~25x to ~10x and 78 from
~11x to ~5x. Below that the remaining cost is the call protocol's
irreducible obligations (window accounting, the walkable frame view,
refcount correctness) - H4 in the ORIGINAL list above, now renumbered
H8 to avoid the collision.

## ⛔ H4 IS BLOCKED BY A SOUNDNESS DISCOVERY (2026-08-12) - maintainer fork

H4 was scoped as "the H1a shape one level up: materialize the call into
a temp, read the temp as a typed leaf". That scoping ASSUMED the static
return type is a runtime guarantee. **IT IS NOT**, and the reason is a
documented language decision, not a bug: FUNCTION SUBTYPING IS
ARITY-ONLY, so a differently-returning function is assignable to a
typed func var. All four programs below COMPILE today:

    func a(int k) { return k; }
    func b(int k) { return 2.5; }
    var g = a; g = b;              # ACCEPTED (arity-only subtyping)
    var s = 0; s = s + g(1);       # -> s = 2.500000   (whole expr PROMOTES)

    func b2(int k) { return "z"; }
    var g2 = a; g2 = b2;
    var s2 = 0; s2 = s2 + g2(1);   # -> TypeErrorEx, caret on g2(1)

    func c(float x) { return x; }
    func d(float x) { return 3; }
    var h = c; h = d;
    var t = 0.0; t = t + h(1.0);   # -> t = 3.000000   (int promotes to float)

So `-dti` proving `g : func(int)->int` does NOT mean g(1) returns an int
at run time, and an unguarded typed tier would compute garbage where the
program today promotes or throws - RULE 1 (no UB) and RULE 2 (an
optimization may not change behaviour) both. Note the two readers CANNOT
paper over it: `read_float_slot` returns a DEFINED 0.0 on a wrong type
(it must not throw - #142's noexcept rule), so the wrong answer would be
silent.

Consequence: H4 needs a runtime tag guard with a DECLINE, and at the
bytecode level that means a NEW type-testing jump opcode plus a
dual-path lowering (typed arm + the existing boxed arm, both writing the
same dst, the call emitted once and shared). That is far more than the
"one function beside try_capture_leaf" the estimate assumed - and it
touches the audited tables, verify_chunk, the disassembler and the myv
format.

### THE THREE OPTIONS (the maintainer picks - a language change is his
### call by the CLAUDE.md rule, and option B is one)

**A. H4 with a bytecode guard + decline.** New opcode (jump-if-tag-not),
dual-path emission. Sound with no language change. Biggest blast radius
of the three; the guard costs one compare per call result.

**B. TIGHTEN FUNCTION SUBTYPING to check the RETURN type (and params),
not just arity.** Then `g = b` above is a COMPILE error, the static
return type becomes a real guarantee, and H4 collapses back to the
one-function form beside try_capture_leaf with NO guard, NO new opcode,
NO dual path. It also permanently unblocks every future optimization
that wants to trust a func value's signature. COST: it REFUSES programs
that run today (the three above), so it is a breaking language change -
which is exactly why it is proposed, not done. Worth measuring how many
corpus programs it would refuse before deciding (expected: zero - the
shapes above are pathological).

**C. Deliver 78's measured win at the GUARD LAYER instead (this is H6,
the float twin of the boxed inline fast tier).** jit.cpp's BinOpV case
already has the exact structure a guard needs - `guard -> fast path ->
j_done -> the interpreter-exact helper` - and the int arm is written; a
float arm is ~100 lines inside that ONE switch case. NO new opcode, NO
bytecode change, NO myv change, sound by the established pattern
(anything not float-float declines to the helper, byte-identical incl.
carets). It removes 78's 190 Ir float chain, which is the SAME cost H4
was going to remove there, and it also helps every genuinely-`dyn` float
loop. It does NOT help the int case (already cheap inline) and does not
generalize to non-arithmetic call results.

RECOMMENDATION: **C now** (it banks 78's win at low risk and is
independently justified), and **B as a design discussion** - if B is
accepted, A becomes unnecessary and H4 lands cheaply and permanently
sound. A is only worth building if B is rejected.

## OPTION B LANDED (2026-08-12) - H4 IS UNBLOCKED AND NEEDS NO GUARD

`static_type_assignable` AND the fixpoint's `join` now both compare the
whole func SIGNATURE (arity + every settled param/return), deferring on
an Unknown / None / dyn component. Commit: "ti: function subtyping
checks the SIGNATURE, not just the arity (option B)".

**THE COMPLETENESS AUDIT** - every route by which a wrongly-typed
function could previously reach a typed call site, re-probed after the
change. Each is now closed, and note they close in THREE different ways,
which is why the audit was worth running rather than assuming:

    var g = a; g = b;              -> TypeMismatchEx (assignable/join)
    var ops = [a, b]; ops[1](1)    -> element join conflicts -> array<dyn>
                                      -> DynRequiredEx (write `dyn` to opt in)
    var d = {0:a, 1:b}; d[1](1)    -> same, via the dict value type
    func pick(n){...return b;}     -> the RETURN type joins to dyn
                                      -> DynRequiredEx
    func use(f){ return f(1); }    -> `use` is a TEMPLATE: monomorphized
       use(a); use(b);                per signature, each instance typed
                                      correctly (prints 1 and 2.500000)
    func[g]() => g(1)              -> the capture keeps g's exact signature

So a call whose static return type is a concrete int/float now really
does return that type at run time. **H4 therefore reduces to what it was
originally scoped as**: a `try_call_leaf` beside `try_capture_leaf` that
materializes the result with the EXISTING call op into a temp and hands
the temp to the typed ops - no guard, no new opcode, no dual-path
lowering, no myv change. The int AND float halves both work, since the
decline that forced the split no longer exists.

Costs nothing at run time and was measured to refuse 0 of 96 corpus
programs. The `dyn` keyword is the documented opt-out for code that
genuinely wants a variable to hold differently-typed functions.
