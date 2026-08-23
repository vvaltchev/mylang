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

## H4 LANDED (2026-08-12) - 78 is 0.74x wall and OUT of the top five

`try_call_leaf` (codegen.cpp), the indirect sibling of the DirectCallExpr
case that has been a typed leaf all along. Measured (interleaved
--baseline, full suite, OPT=1 ASSERTS=0):

    78_typed_param_call   0.74x wall   my/cpp 10.87x -> 8.02x
    suite geomean cur/base 1.001x (untouched benches span 0.88-1.11x)

**NO RUNTIME GUARD** - option B is what makes that legal. If function
subtyping is ever loosened back toward arity-only, this tier is the
first thing that breaks, silently.

THE PLACEMENT TOOK THREE TRIES, each caught by a test and not by
reading, which is the durable part of this entry:

 1. **EARLY in compile_int_expr -> INFINITE RECURSION.**
    `compile_boxed_expr` DELEGATES back to the typed compilers for a
    th==i/f node, so the leaf called itself (ASan stack overflow through
    try_call_leaf -> compile_boxed_expr -> compile_int_expr). Hence the
    `allow_typed` opt-out parameter, which exists for this one caller.
 2. **LAST-resort -> DEAD CODE.** compile_int_expr returns false at
    `if (!t) return false;` for any non-TypedScalarExpr, long before the
    function tail - so a CallExpr never reached the hook and 78 went
    back to `bin.v` while the suite stayed green.
 3. **After the specialized paths but UNNARROWED -> ate MathFnV.**
    `s += sqrt(i)` fell from the typed MathFnV to the generic
    CallBuiltinV marshal (vm_codegen_shapes case 14, watched failing).

Final: after every specialized typed path, before the TypedScalarExpr
cast. The Direct*-form exclusion in the leaf is DEFENSIVE, not
load-bearing - removing it keeps the suite green at this position
(watched), and the comment says so rather than claiming credit the
placement earns.

**THE TEST'S OWN TRAP, worth more than the optimization.** Counting
`IntBin`/`FloatBin` made the shape test read 0 and pass vacuously,
because the plain op disappears TWICE before vm_compile returns: a
counted loop FUSES the accumulate into `IntAddStep` (#9), and
`specialize_arith_ops` rewrites the rest into the B1/B2 family (the
float case emits **FloatAddRR**). That is THE AUDIT-TABLE STAGE TRAP in
test form - the same shape CLAUDE.md records for visit_use_def and
op_writes_scalar, and the second time it has bitten a test in this arc
(H1 hit it too). **A test that names an opcode must name the one that
SURVIVES to the stage it inspects.**

Four behaviour tests pin that a typed leaf did not change WHEN the call
runs: short-circuit under `&&`, only the taken ternary arm, two operands
left-to-right, and the int+float values.

### The top five after H4

    76_funcval_dispatch   17.73x   (H5 - the value-model assign - is its item)
    75_indexed_unpack     11.09x   (the single-var foreach bind, #160)
    63_closures           10.89x
    73_multi_unpack        9.00x
    11_closure_counter     8.64x
    ...
    78_typed_param_call    8.02x   (was 10.87x - out of the top five)

76 is now the clear outlier and its cost is NOT the call protocol alone:
~184 Ir/iteration is the type-erased assign triple (dtor ->
default_ctor -> copy_assign) around one array-handle argument. That is
H5, and H3 is its proof of concept.

## ⛔ H5 INCREMENT 1 ATTEMPTED AND REVERTED (2026-08-12) - a NEGATIVE
## result worth keeping: the assign's cost is WORK, not DISPATCH

The plan said the value-model assign "costs 37-100 Ir where ~10 is the
work" and named devirtualization as the fix. **That reading of the
callgrind attribution was WRONG, and the experiment says so three
times.**

WHAT WAS BUILT (evalvalue.h, reverted): `SharedStr` and `SharedArrayObj`
are COMPLETE types in evalvalue.h, so a same-type assign of either can
call its own `operator=` DIRECTLY instead of through `type->copy_assign`
- an indirect call the compiler can neither inline nor optimize across.
Plus the type-CHANGE path collapsed from three indirect calls
(dtor -> default_ctor -> copy_assign) to two (dtor -> copy_ctor).

MEASURED on 76_funcval_dispatch (Ir/iteration, OPT=1 ASSERTS=0, the
scale-1-vs-3 delta), against a pre-H5 baseline of **734**:

    devirtualize str+arr, tests before the trivial split   741  (+1.0%)
    ...with the trivial split hoisted first                765  (+4.2%)

WORSE, twice, and MORE devirtualization made it WORSE. The profile of
the second attempt says exactly why:

    TypeImpl<SharedArrayObj>::copy_ctor            90 Ir   <- REPLACED
    (the default_ctor + copy_assign it replaced)   84 Ir      a CHEAPER pair
    EvalValue::operator=(&&) inline body          171 Ir
    EvalValue::operator=(const &) inline body     108 Ir

Two lessons, both general:

1. **`copy_ctor` is NOT cheaper than `default_ctor + copy_assign` here.**
   The "one indirect call instead of two" arithmetic ignored that the
   ctor does MORE work (a slice registration path the assign's
   `if (slice)` skips outright when both sides are non-slices, which is
   the common case).
2. **The 37 Ir inside copy_assign is mostly REAL WORK** - an
   intrusive_ptr release+retain (two refcount RMWs with their
   delete-checks), two slice tests, four field copies - NOT dispatch
   overhead. The indirect call is ~5 Ir of it. Removing 5 Ir of dispatch
   while inlining ~30 Ir of body into every one of the MANY assignment
   sites is a net loss, and it grows with how many sites inline it.

So **76's 184 Ir of array-handle traffic is not reducible by attacking
the assign's dispatch**. What is left to try, in the order I would try
it, none of them started:
 - **cut the NUMBER of assignments**, not their cost: 76 copies the `st`
   handle TWICE per iteration (arg bind + a release/rebind round trip).
   One of those is plausibly removable at the call protocol level - a
   reference argument that the callee only READS need not be a fresh
   retained handle at all if the caller's slot outlives the call, which
   it does.
 - an INLINE emitted tier for `jit_bind_ref_arg` (78 Ir) in the JIT,
   mirroring #92/#93 - emit the retain + 4 stores at the call site.
 - the refcount itself (N7 territory), which is the value model's floor.

METHOD NOTE: this is the "DISTRUST A SURPRISING RESULT" rule paying off
in the *other* direction - the surprise was a REGRESSION where the
mechanism predicted a win, and taking it seriously (rather than shipping
on the plausible story) is what produced the finding. Measure the
mechanism, not the attribution.

## DIRECTION 1 INVESTIGATED (2026-08-12): the double retain is REAL,
## and the fix is an OWNERSHIP change, not a copy deletion

Confirmed with caller-attributed callgrind: 76 makes exactly TWO
array-handle copies per iteration, and they are

    jit_bind_ref_arg  1.00/iter   (the callee's param slot)
    jit_move          1.00/iter   (the ARGUMENT STAGING slot)

The bytecode names the second one:

    23  i.bin        r6 = i % 2
    24  load.elem.v  fn = ops[r6]
    25  move         r6 = st        <-- retain #1, into the arg run
    26  move         r7 = i
    27  call.val     _ = fn(r6, r7) <-- retain #2, into the callee frame

`st` is retained twice and released twice for ONE call. The staging move
exists because the call protocol reads its arguments from a CONTIGUOUS
register run, so an argument that lives in an arbitrary slot has to be
copied adjacent to its siblings first. (The callee itself is not the
problem - `add_op$0` is fully typed and native: load.elem.i / i.bin /
store.elem.i.)

**WHY THE STAGING RETAIN LOOKS REMOVABLE.** Between the `move` and the
bind, NO user code runs - no call, no allocation, no unwinding - so the
source slot cannot change and the value cannot be freed. The staging
slot only has to carry the bits across that window; the REAL reference
is taken by jit_bind_ref_arg. So the staging copy can in principle be a
raw BORROW (copy the 24 bytes, no refcount bump). Note this is NOT the
borrow H2 rejected: there the callee had to stay alive ACROSS the call,
here the window contains nothing at all.

**THE HAZARD, which is why this is not a small change.** The staging
slot is a frame TEMP, and the frame's `ref_slots` machinery releases
every slot that may hold a reference (jit_release_slot, frame teardown,
slot reuse). A borrowed staging slot listed in ref_slots would be
OVER-RELEASED - a use-after-free, not a wrong number. So the change is
not "delete a copy", it is "teach the call protocol that an argument
staging slot is NON-OWNING for the window it exists", which means:
  - the emitter must exclude such slots from ref_slots (or mark them),
    and `jit_ret_audit`'s ref_slots contract has to agree;
  - the DECLINE path matters - an argument that is NOT a stable slot (a
    computed temp, a call result) must keep the owning copy;
  - the exception path has to be checked: if the bind throws (a coerce
    error), the staging slot must not be released either.

SIZED: ~1 retain + 1 release + 1 type-erased assign per reference
argument, i.e. of 76's 734 Ir/iteration the `jit_move` line is ~81 Ir
plus its share of the 106 Ir triple. Worth doing, but it is call-protocol
ownership surgery and belongs in its own increment with the ref_slots
audit in scope - NOT bolted onto a measurement session.

NOT STARTED. The investigation above is the deliverable; the next
session should start from the ref_slots contract, not from the emitter.

## #162 LANDED (66e8562): THE IN-PLACE ARGUMENT - the staging copy is
## DELETED, and the ownership surgery the investigation predicted was
## NOT NEEDED

The investigation above framed direction 1 as "make the argument staging
slot NON-OWNING": borrow into it, then neutralise it, with `ref_slots`,
the release scan and every throwing exit in scope. **That framing was
wrong in a useful way.** The staging slot only has to stop being WRITTEN,
not stop being OWNING:

    23  i.bin        r6 = i % 2
    24  load.elem.v  fn = ops[r6]
    25  move         r6 = st        <-- NO LONGER EMITTED
    26  move         r7 = i         <-- kept (trivial: 2 stores, cheap)
    27  call.val     _ = fn(r6, r7) <-- arg 0 read from `st` directly

Skipping the write leaves the run slot holding whatever it held, still
correctly owned by whoever wrote it. So `ref_slots` is untouched, the
release scan is untouched, and there is no exception path to reason
about. The copy is not made cheaper - it does not happen. A borrow would
have deleted ONE retain/release pair (~15 Ir) and cost the whole
ownership discipline; deleting the move takes the helper call and the
type-erased triple with it.

SOUND in one sentence: the caller slot and the run slot hold the SAME
value, so reading either is correct as long as nothing writes the caller
slot in between - and between the staging moves and the call, nothing
writes anything but run slots. That is also why it survives a branch or
a fragment entry landing anywhere in the sequence.

THE ONE READER LEFT is every arm that hands the call to C++: a guard
decline, the depth-cap SWITCH, and the BAIL whose status 1 resumes the
INTERPRETED call op (jit_call_sync_core's documented idempotent bail).
They converge on ONE join, and `jit_stage_args` materialises the run
there. jit_sync_postexit was checked and cannot bail - it returns 0 or 2,
so its exit_pc is always a re-raise.

JIT-DERIVED, never a bytecode fact: recognized from the instruction
sequence at emit time, so the interpreter is untouched, no myv version
moves, and a hostile image cannot assert it (#137's layering could not
verify "this slot is an argument").

THE GATE I DID NOT ANTICIPATE, and the test that taught it: the coercing
arm widens bool->int / int->float **in the argument temp**, whose
soundness note reads "emit_args_range gives every argument a FRESH temp"
- which a fused argument is not, so widening one would write the
CALLER'S VARIABLE. My first version declined that arm for fused args and
took the inline widening away from every named-local argument;
`jit_bind_widen` failed immediately (its widening argument is a plain int
loop counter). The fix is to fuse ONLY `ref_slots` members: a reference
at a numeric parameter already declined there, so the arm's behaviour is
now identical to before - and it is the right VALUE gate anyway, since a
trivial argument's staging move is already the inline two-store path.

MEASURED, OPT=1 ASSERTS=0 on both sides, baseline = dab3f9b:
 - **76 per-iteration Ir 734 -> 588 (-19.9%)** (the scale-1-vs-3 delta,
   so process startup, compile time and JIT warmup are excluded from
   both sides); whole-program -19.4% / -19.7% at scale 1 / 3.
 - **wall-clock 0.73x** (0.036 -> 0.026s), from the interleaved
   `--baseline` full-suite run - the trustworthy form, both binaries
   timed in one session.
 - **my/cpp 17.7x -> 12.42x**; 76 is no longer the worst bench.
 - suite geomean cur/base **0.992x**, every other bench inside
   0.94-1.05x. my/python geomean 11.08x over 77 pairs.
   (14_array_subscript tripped the variance gate at 6.6% and its number
   is unreliable either way - unrelated to this change, which cannot
   reach a subscript loop.)

SABOTAGE (all watched):
 - ref_slots gate removed -> `jit_bind_widen` fails AND
   arg_inplace_shapes reports the int local fused (1898/1900);
 - cold-arm materialisation removed -> the ref-arg bind test fails and
   the suite ABORTS on a downstream assertion; the cold shape run alone
   gives InternalErrorEx (the C++ tier read an unwritten run);
 - named-local gate removed -> NOTHING fails (-rt and the corpus stay
   green). Recorded as defensive, not proven, in the code.


## H6 / H7 / H8 LANDED (2026-08-13) - what each actually bought, and the
## two things the plan above got wrong

**H6 - the boxed inline tier's FLOAT arm (6c55f84).** The plan sized it
off 78_typed_param_call; H4 has since typed that call result, so the
motivation was stale. Measuring reach FIRST said `boxed_slow_f` is
**ZERO across bench/my + samples** - and a two-line probe produced
8,000,000. Real shape, absent corpus, so 79_dyn_float was added and the
arm built: **0.20x wall** on it. Remaining siblings, sized by the new
`boxed_slow_m` counter: the MIXED int/float arm, float `%`, eq/noteq,
UnaryV.

**H7 - the unpack (57cbdd7).** The plan asked for an emitted inline
tier. Measuring first said the cost was not where a tier would attack
it: of ~222 Ir/row, only ~44 was the refcount traffic the bind exists to
do; the rest was a per-element call re-reading `skind()`/`offset()` and
re-deriving the element vector. Hoisting the dispatch out of the loop -
**no emitted code at all** - took 75_indexed_unpack to **0.83x wall,
-19.4% Ir** (~165 Ir/row, the plan's 150-200 target). 73_multi_unpack
-7.1% from the sibling fix. The emitted navigation tier is sized (~105
Ir/row) and NOT built.

**H8 inc 1 - the depth cap is the segment budget (ae6a5be).** The full
H8 is ~28 emitted instructions per call; inc 1 removed the ~9 that need
no fork (the `used` counter, the cap test, and the segs[] re-derivation
on both sides). **10_recursion_deep -4.74% Ir / 0.93x wall**;
09_fib_recursive and 08_func_call exactly 0.00%, because their calls do
not reach the emitted push at all. It also closed a real hole: the
depth cap - the catchable StackOverflowEx that the whole segmented slot
stack exists to provide - **had no test anywhere**, and could not have
one in `-rt` (the cap is a per-process static). Three driver checks
cover it now.

**INC 2 IS A FORK FOR THE MAINTAINER, NOT A QUEUED TASK.** Putting the
window on the native stack removes the remaining ~13 instructions, and
four things depend on the current shape: the G1 walker's
`seg_top_before` chain (Nets 2 and 3 verify it field for field, so the
ORACLE has to be re-derived BEFORE the change), StackOverflowEx's
mechanism, helper visibility across callbacks, and slot construction.
Full statement in docs/jit-optimizations.md's H8 entry.

**AND A WRONG-CODE BUG FELL OUT OF H6 (d9f4d1c)**, unrelated to
performance: `op_writes_pure_target` still listed `LogV`, which stopped
being a single-producer op when #138 gave `&&`/`||` real short-circuit
branches. `print(s > 5 && s < 6)` with a dyn `s` printed **`3`** under
the VM and `false` in the tree-walker. Latent in 8 corpus programs
(bytecode changed, output did not). The general lesson is in CLAUDE.md:
when you change HOW a construct lowers, re-audit every table that
classifies its OPCODE - the entry does not have to be edited to become
false.

## THE CALLBACK ENTRY: `VmInvoker::call` LANDED; THE TYPED BIND INSIDE IT
## WAS BUILT TWICE, MEASURED AND REJECTED (2026-08-14)

Two separable ideas were tried together, and only after splitting them
did the data make sense. Keep them separate when reading this.

### WHAT LANDED: one `call(args...)` entry, boxing each argument ONCE

`VmInvoker::call(args...)` is now the single entry every higher-order
builtin uses. It takes the callback's arguments in whatever C++ types the
builtin holds them in, boxes each exactly once into an argv run, and
picks the tier itself: the prepared window (`invoke`) or `eval_func`
when there is no activation to run a boundary frame on. All FIVE call
sites (sort's comparator, find's key, `make_array`, `make_dict`,
`map`/`filter`) collapsed to one `inv.call(...)` line with no ladder of
their own - which was the maintainer's brief, since more higher-order
builtins are coming and each was repeating that ladder slightly
differently.

**The win is not the tidy-up, it is the DOUBLE BOXING it removes.**
`sort` reached its invoker through a `cmp2(EvalValue, EvalValue)` lambda
shared by the five storage arms, so one flat-int comparison built TWO
EvalValues for `cmp2`'s parameters and then COPIED them into a
two-element argv - four constructions per comparison. `call(a, b)` takes
the raw `int_type`s and builds the argv directly: two.

MEASURED (`OPT=1 ASSERTS=0` both sides, interleaved `--baseline`):
 - 34_sort_custom_cmp **-16.2% instructions** (893.6M -> 748.9M) and
   **0.89x** wall clock;
 - 12_higher_order **0.87x**;
 - full suite geomean **1.002x** (flat), 33_sort_ints - the same sort
   with no comparator - flat, as it must be.

### WHAT DID NOT LAND: binding the raw scalar into the window slot

The obvious next step is to skip the argv entirely: write the
`int_type`/`float_type`/`bool` straight into the callee's window slot
from its static C++ type. It was built in FOUR shapes and every one lost
on the wall clock. Do not rebuild it without reading this.

REACH WAS PROVEN, so none of this is a measurement of dead code:
`MYLANG_JITSTATS` now reports `cb_prepared`/`cb_fallback`, and with the
typed bind in, 34_sort_custom_cmp took it **2,820,290 of 2,820,290**
comparisons.

THE FOUR SHAPES, all measured against the same baseline:
 1. bind inlined at the call site, body shared through a file-local
    ALWAYS-INLINE helper: **1.20x slower**;
 2. the same, plus a compile-time gate restricting the typed bind to raw
    scalars: **1.22x**;
 3. the same, plus `ML_NOINLINE` on the body: **1.21x**;
 4. the whole typed entry out-of-line (`ML_NOINLINE call_typed<...>`),
    so the hot loop sees exactly one call: **1.21x**.

THE CONTROL THAT SETTLED IT, and it inverted the diagnosis: the SAME
refactored source with the typed path disabled at compile time measured
**0.90x** - faster than baseline - on 34_sort_custom_cmp. So the refactor
was always the win and the typed bind was always the loss; the first
attempt's numbers had them summed and read as one result.

EVERY SIMULABLE METRIC SAID IT SHOULD BE FASTER. Cachegrind on shape 1:
-28.2% instructions, -28% branches (122.1M -> 88.3M), D1 misses
464,030 -> 463,098 and LL 49,031 -> 49,043 (identical), mispredicts
+2.5%. That combination - better on everything a simulator models, worse
on the clock - is this codebase's known front-end/code-layout signature
(see the memory note on the VM dispatch front-end regression, where a
+0.5% instruction delta was amplified 24x in wall clock). This box is
WSL2 and has no PMU, so the front-end cannot be measured directly here;
if someone wants to close this out properly, that is the tool needed.

TWO MORE TRAPS WORTH KEEPING, both cost real time:
 - **The gate is `fast_bind`, and the callee's INFERRED TYPES never
   enter it.** The first version gated on `ParamDesc::proven_type` and
   engaged ZERO times, for a structural reason: `proven_type` is stamped
   only for a function NEVER USED AS A VALUE, and a callback is by
   definition a function used as a value.
 - **A baseline BINARY and a baseline PROFILE are not the same thing.**
   The first reading, "-29.5% Ir", was taken against callgrind `.out`
   files captured before the constant-divisor strength reduction landed;
   bench 34 fills its array with an LCG containing `x % 2147483647`, so
   most of that "win" belonged to the previous commit.

### An unrelated observation, NOT chased

A `float`-annotated callback param does not appear to receive a coerced
float: with the `fast_bind` gate REMOVED (so the coercion is skipped
entirely), `map(func(float x) => x / 2, <array of ints>)` still prints
0.5, and a `dyn` global assigned from such a param reads back as an INT
on the CORRECT path too. It behaves identically on the pre-existing
boxed path, so it is not the callback work's doing - but it means a
value assertion on that decline is vacuous, and the `-rt` case pins it
on the counter only and says so.


## WHERE 76_funcval_dispatch (11.1x) AND 63_closures (10.6x) ACTUALLY SPEND
## THEIR TIME (measured 2026-08-14)

Both were profiled with callgrind at `OPT=1 ASSERTS=0`, per-iteration
figures taken as the scale-1 vs scale-3 DELTA so compile time is
excluded. `MYLANG_JITSTATS` was read on both first, because the
interesting result is what it RULES OUT.

### THE DISPATCH TIERS ARE ALREADY OPTIMAL - THIS IS NOT A JIT GAP

76_funcval_dispatch, 1,000,000 iterations:

    sync_inline      999999      arg_inplace      999999
    callee_cache     499999      callee_cache2    499999
    ref_arg_binds    999999      bind_coerce      0
    divmagic        1000000      arg_stage        1

Every call takes the inline sync tier; #162's in-place reference
argument binding fires on every call; the 2-entry polymorphic callee
cache hits every time (the target alternates, so both entries are
live). There is no tier left to reach. 63_closures is the same picture
(`callee_cache` 999995 of 1M).

**So the remaining cost is the VALUE MODEL, not dispatch** - which is
task #60's territory, and it is why neither bench moved when the call
protocol did.

### 76: 600 Ir PER ITERATION, against ~10 for the C++ twin

    jit_load_elem_value                 39   \  reading ops[i % 2]
    arr_elem_at                         25   |  and boxing the
    TypeImpl<FuncObject>::move_assign   14   |  FuncObject handle
    LValue::put(EvalValue&&)            25   /  into `fn`      = 103
    jit_bind_ref_arg                    26   \  binding `st`
    TypeImpl<SharedArrayObj>::copy_asgn 18   /  (a retain)     =  44
    jit_release_slot                    30      the matching releases
    ------------------------------------------------------------
    helpers                            177  (29.5%)
    emitted native code               ~277  (46%) - the inline call
                                                    guard sequence

C++ loads a function pointer and calls it. MyLang boxes a refcounted
handle out of an array, stores it in a slot (retain + release), retains
the array argument, calls, and releases both.

### 63: 1816 Ir PER ITERATION (2 closure creations + 3 calls)

    FuncObject::FuncObject (3 inlined copies)  198   \ closure
    jit_make_closure (2 copies)                 82   / creation = 280
    jit_ret_norec                              146     ~49 per return
    EvalValue::operator=(&&)                   120

FuncObject already has `ML_POOL_NEW_DELETE`, so this is NOT malloc -
it is the per-closure field/capture-slot setup, paid twice an
iteration. (The `__dynamic_cast` + `__strcmp_avx2` in the scale-1
profile are COMPILE time - they stay constant at scale 3.)

### THE TWO LEVERS, AND THEY NEED THE SAME NEW ANALYSIS

Both of 76's big items are blocked on the same missing fact: **what a
CALLEE can do to what it is handed.**

 1. **A non-escaping reference argument should bind with NO retain.**
    For a SYNC call the caller's slot holds the reference for the whole
    call, so the callee's slot needs its own reference only if the
    reference can OUTLIVE the call - stored into a global, a container,
    a struct field, a closure capture, or returned. `add_op(st, x)`
    only subscripts `st`; it cannot escape. Worth ~74 Ir on 76 (12%),
    and it generalises to every `compute(arr)` / `process(data)` call
    in the language.
 2. **A func value read out of a never-written array should be
    BORROWED, not boxed.** `ops[i % 2]` immediately feeds a call; the
    LoadElem2 precedent (borrow the row, consume it inside one op)
    applies - EXCEPT that here real user code runs in between, so the
    borrow is only sound if the callee cannot mutate `ops` and drop the
    last reference to the function being executed. Worth ~103 Ir (17%).

Together they take 76 from **11.1x to roughly 7.6x**. 63 needs the
closure-creation path as well (280 Ir, 15%) to reach 8x, and its
remaining half is the call/return protocol itself.

**The shared prerequisite is a per-function ESCAPE/MUTATION SUMMARY** -
for each parameter and each reachable global, can this function (or
anything it calls) let it escape, or mutate it? `func_mutates_input`
(resolver.cpp) already computes the mutation half for purity over a
taint analysis; this is its transitive, escape-aware sibling, and it
would want to live beside it and be stamped on the FuncDescriptor.

⛔ THAT IS A NEW WHOLE-PROGRAM ANALYSIS WITH A USE-AFTER-FREE FAILURE
MODE, so it is NOT started unilaterally - it needs the maintainer's
sign-off on the design before any code. One contained piece needs no
new analysis and could go first as a warm-up, though it is small: the
per-argument bind type-check emitted at every call site is provably
dead here (`bind_coerce` and `bind_widen` are BOTH 0 across 1,000,000
calls, because the value-template instance's params are typed and the
arguments' static types already match), worth ~3%.


## #93 INCREMENT 1: THE PARAMETER ESCAPE ANALYSIS IS BUILT - AND ITS REACH
## SAYS THE CONSUMER IS NOT WORTH BUILDING UNTIL INCREMENT 2 (2026-08-14)

`compute_noescape_params` (resolver.cpp) answers, per parameter, "can the
reference this is bound to still be reachable after the call returns?",
and stamps `FuncDescriptor::noescape_params`. It is correct, tested and
sabotage-verified. It has NO consumer, deliberately - and the reach
number below is why that turned out to be the right call rather than a
staging decision.

**MEASURED REACH: 4 parameters in 76_funcval_dispatch, and FIVE across
all of samples/ + bench/my. Zero in 63_closures, zero in 46_matrix_mult.**

The rule that costs it is "the body contains NO call of any kind", which
makes the pass non-transitive and therefore cheap and obviously sound -
but `len(a)` is a call, so the single most common shape in the language
(`func f(a) { for (var i = 0; i < len(a); i++) ... }`) is excluded, and
with it essentially every real function. Wiring the borrow to THIS answer
would buy ~12% on one benchmark and nothing anywhere else.

**So increment 2 is not optional polish, it is the whole feature:** make
the analysis TRANSITIVE over the call graph (a fixpoint, as
`build_reachable_reads` already does for the unbound-call prover - and
for the same reason: mutual recursion makes the graph cyclic, so there is
no traversal order). A call then propagates its callee's per-parameter
escape bits instead of ending the analysis, and a builtin needs a small
audited table of "does this store its argument" (`append`/`push`/`insert`
do; `len`/`abs`/`str` do not) - which is exactly the kind of table
CLAUDE.md's audit-table trap warns about, so it wants the assert-the-
table-covers-its-input treatment.

TWO HAZARDS THE CONSUMER STILL OWES, both found by reading the write path
and both recorded on the pass itself:
 1. `try_flat_subscript_store` and `get_value_for_put` gate
    `clone_aliased_slices` on `use_count() > 1`. A borrow does not bump
    the count, so that guard would stop firing where it fires today - a
    live slice of the argument would observe the write. RULE 2 violation.
    The predicate has to move off `use_count` and onto "this array
    actually has live slices" first, which is both more precise and makes
    the write path independent of how many handles exist.
 2. An element write through a SLICE detaches it by ASSIGNING into the
    handle, which would release a reference a borrowed slot never took -
    a refcount underflow. Sliceness is a runtime property, so the bind
    has to decide per call and record it (a borrow mask on the window
    record, which `pop_window` then consults).

THREE THINGS THE TEST CAUGHT, all on their first run, and the last two
are the general lesson:
 - the expected mask for `dst[0] = a` was written backwards by me: the
   STORED value escapes, the destination does not;
 - **the pass was stamped in the wrong PIPELINE STAGE.** It first ran in
   `process_function`, beside its sibling `func_mutates_input` - but the
   global-write rule reads `SymKind::global`, which pass 2 has not
   stamped yet at that point, so `func f(a) { g = [9]; a[0] = 1; }` came
   back marking `a` safe. It runs at the END of `resolve_names` now. That
   is the audit-table stage trap in its usual shape, and here the failure
   direction is a use-after-free;
 - **a test case for "the param is passed to another function" was
   VACUOUS**: the AST inliner spliced the callee into the caller, leaving
   a body with no call and a genuinely non-escaping param. The case needs
   a callee the inliner refuses (one that reassigns a scalar param).

AND TWO OF THE THREE GUARDS ARE DEFENSIVE, NOT PROVEN - said plainly
because the sabotage runs say so: deleting the CallExpr test or the
`slot_writes` test leaves the whole table green, because the
base-position rule clears those bits first. Only one row in the table
fails when the CallExpr test is removed (the call that never mentions the
param, added for exactly that reason); no row fails without the
`slot_writes` test at all. Both stay - the failure direction is a
use-after-free and the rules answer different questions - but nobody
should believe they are pinned.
