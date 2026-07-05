# VM fallback elimination + optimization — road to 5× CPython

**Goal:** **5× is the FLOOR, not the target.** The per-benchmark table shows the
native benches already run 5–7× CPython, so the ceiling is well above 5× once
the fallbacks are gone. **No possible optimization is skipped** — pursue every
category below and every Part-C idea until the VM's own dispatch is the only
floor left.

## Progress tracker (update every step — strike a row when it lands)

Hot fallback categories, most-benchmarks-first. Strike + date a row when its op
lands and the differential/bench confirm it; keep it (struck) for the record.

| Cat | Construct | Benches | Fix (Part B) | Status |
|---|---|---|---|---|
| ~~**B**~~ | ~~bool array r/w~~ | ~~43,56,57~~ | ~~P1~~ | ✅ 43:.48 56:.15 |
| ~~D1~~ | ~~dict store~~ | 5 | ~~P2~~ | ✅ .55/.43 |
| ~~D1m~~ | ~~dict MEMBER store `d.k=v`~~ | (rare) | P2b | ✅ DictStore (string key) |
| ~~S1m~~ | ~~struct field store `s.f=v`~~ | 58,65 | — | ✅ StoreMemberV (POD/boxed) |
| ~~N2~~ | ~~nested store `a[i][j]=v`~~ | 68,matrix | — | ✅ StoreElem2V (flat+general) |
| ~~G/C~~ | ~~global/capture container base~~ | — | — | ✅ as_container_base + vm_store_base |
| ~~DYN~~ | ~~dyn/unproven-base store~~ | — | — | ✅ universal StoreElemValue (flat via core) |
| ~~D2~~ | ~~typed dict read~~ | ~~25,24~~ | ~~P3~~ | ✅ 25:.53 24:.17 |
| ~~F1~~ | ~~foreach single-var gen arr~~ | (grp) | — | ✅ box-free LoadElem.v |
| ~~F2~~ | ~~foreach INDEXED 2-var~~ | 19 | — | ✅ box-free (see "redo") |
| ~~F3~~ | foreach UNPACK `x,y in pairs` | 20 | — | ✅ UnpackElem 0.87→0.73 |
| ~~D3~~ | dict `foreach(k,v in d)` | 26,47,62 | — | ✅ live iterator 62:.64 |
| ~~G~~ | ~~general array store~~ | 5 | ~~P4~~ | ✅ 32:.67 20:.62 |
| ~~S~~ | string element `s[i]`, build | 29,30,31 | — | ✅ native; 30 len-bound |
| ~~M~~ | multi-assign literal `a,b=[..]` | 22 | — | ✅ array-elide 0.89→0.18 |
| ~~Sl~~ | slice `a[i:j]`/`s[i:j]` | 15,16,29 | — | ✅ SliceV .50-.78 |
| ~~C~~ | ~~closure/indirect call `c()`~~ | 11 | — | ✅ CallValueV 1.09→1.00 |
| ~~R~~ | global write `g=`/`+=`/`++` | — | — | ✅ StoreGlobalV (0-bench) |
| ~~Cap~~ | closure capture `cap++` | 11,63 | — | ✅ StoreCaptureV (0-bench) |
| ~~MkC~~ | closure `<lambda>` create | 11,63 | — | ✅ MakeClosureV (node-free) |
| ~~FDecl~~ | top-level `func f(){}` | all | — | ✅ MakeClosureV+StoreGlobalV |
| ~~SDecl~~ | `struct P{}` decl bind | 58 | — | ✅ LoadConstV+StoreGlobalV |
| ~~SCtor~~ | `P(x,y)` construct | 64 | 0.75x | ✅ StructCtorV (typed POD) |
| ~~SFe~~ | struct foreach `p.x` | 65 | 0.27x | ✅ LoadStructField (direct) |
| ~~I++~~ | subscript `a[i]++`/`d[k]++` | 47 | — | ✅ StoreElem/DictStore±1 |
| ~~AF~~ | DictStore/StoreElemV AST-free | — | — | ✅ vm_subscript_store loc |
| **A** | `push(a,i)` value self-eval | 13 | P10a | native already (noise) |
| ~~Lit~~ | array/dict literal `[..]`/`{}` | 20,22,46 | — | ✅ MakeArr/DictV |
| ~~LitO~~ | const-literal decl | many | — | ✅ LoadLiteralObjV 76→55 |
| **X** | try/catch + C++ `throw` | 42 (24.5×) | P8 | LAST (VM-level exc) |

Plus the Part-C native-but-slow work (typed reads, computed-goto dispatch,
arg-view builtin ABI, ...) — none skipped.

## Remaining work (current — 2026-07-07)

The tracker rows above are kept current (struck as they land). The Part A/B/C
PROSE further down is the ORIGINAL roadmap and is now STALE for the DONE items:
P1-P7 + P9 (bool arrays, dict store/read, general store, ALL foreach forms incl.
struct, strings, slices, multi-assign, closures/func decls, structs) have all
landed — see the tracker, not the prose. What genuinely REMAINS, most-valuable-
first:

1. **Builtin ABI holdouts.** ~~`sort`/`rev_sort`/`reverse`~~ **DONE**
   (2026-07-08) — migrated to a const-capable lvalue ABI
   (`make_const_builtin_lv` + a shared `sort_core`/`reverse_core`, `func` for
   the tree-walker + `func_lv` for the VM's `CallBuiltinLV`; added to
   `is_lvalue_arg_builtin`). `sort(a)`/`reverse(a)` are `call.blt.lv` now;
   `21_array_reverse`'s reverse-in-a-loop goes native. See
   `builtin-abi-migration.md`. ~~`map`/`filter`~~ **DONE** (2026-07-08): they
   **validate arg0 before evaluating arg1** (the eager value ABI reorders that),
   so they stay `func` for the tree-walker but the VM lowers them natively as a
   `CheckFuncV` (validate arg0, throw before arg1's code) + a `MapFilterV`
   calling the shared `vm_map_filter` core. 35_map_filter is now 0 fallbacks.
   The builtin floor is now ONLY the inherently-node-based AST builtins
   (`defined`/`isconst`/`type`/`show`/...), freed later by the AST-free builtin
   loc handle (`vm-ast-free.md` Steps 1-3).

   **AUDIT CORRECTION (2026-07-08):** the earlier per-bench `-vd` fallback
   counts were INFLATED by **dead template bodies** — a `func f` whose calls all
   redirect to a native instance `func f$0` (the template is never compiled at
   runtime). E.g. 43_sieve's 5 `eval.stmt` are all in the `compute_primes`
   template; the `compute_primes$0` instance (the hot path) is fully native.
   Excluding dead templates, the genuine LIVE fallbacks are: map/filter (35);
   ~~a scalar-spread multi-assign `a,b=0` (06/22)~~ **DONE** (2026-07-08,
   `try_multi_scalar_spread` — compile the scalar once, MoveV to each target);
   ~~a big const array literal decl (48/52)~~ **DONE** (2026-07-08, `DeclConstV`
   — materialize the rvalue then bind the slot as a CONST LValue so a later
   rebind still throws; one-time, so not a perf mover);
   ~~a `var dyn` foreach reduction (48_heavy)~~ **DONE** (2026-07-08,
   `ForeachDynInit`/`ForeachDynNext` — a runtime-dispatching box-free single-var
   iterator, array element / dict key; measured on the dedicated non-folding
   `66_dyn_foreach`); exceptions (42). Audit with: for each chunk, a `name`
   is a dead template iff some other chunk is `name$<digits>`.

2. **Free the AST - drop `Instr::node`** (the node-field goal). The AST-free
   foundations exist (loc side table, const / member-key / struct-def / closure-
   def pools). The remaining `node` users are: the **builtin call ops**
   (`CallBuiltinV`/`LV`/`LVElem` + `EmplaceStruct` - they hold the args
   `ExprList` + the per-arg caret; the **builtin loc-handle refactor** is the
   step that frees them) and, inherently, the **fallback ops** (`EvalStmt`/
   `JumpIfFalse` - they hold the node to re-enter `node->eval`). So the field
   can only come off once (a) the builtin ops migrate to a loc-handle + an
   AST-free arg source, AND (b) every construct is native (no `EvalStmt`/
   `JumpIfFalse` left). This is the FINAL structural step.

3. **Exceptions (X / P8) - LAST.** `try`/`catch`/`throw` are `EvalStmt`; every
   `throw` is a C++ throw (bench 42, 24.5x). Needs VM-level exception dispatch
   (a handler stack + a pending-exception jump). One bench (~5% geomean) but the
   stated endgame + the single most-dramatic per-bench win.

4. **Small residual fallbacks** (low value): ~~a dict MEMBER store `d.k=v`
   (D1m)~~ ✅ (DictStore, string key); ~~a struct field store `s.f=v` (S1m)~~ ✅
   (StoreMemberV, POD byte store / boxed-field slot_rmw — gated on the
   inferencer's `MemberExpr::base_struct`, base a slotted local; a flat-array
   element `a[i].x=v` base is a pre-existing tree-walker NotLValueEx);
   ~~a nested store `a[i][j]=v`~~ ✅ (StoreElem2V — reads `a[i]` as a reference
   then stores `[j]`; a FLAT inner via the shared `flat_store_core`, a GENERAL
   inner via the element LValue + slot_rmw; matrix benches 0-fallback, VM ~2.2×
   the tree-walker on the nested workload); ~~a GLOBAL/capture container base~~
   ✅ (`as_container_base` returns a slot KIND 0/1/2, the store ops carry it in
   `in.target`, `vm_store_base` forms the base LValue* from the frame / global
   table / capture vector — so `a[i]=v` / `d[k]=v` / `s.f=v` target a top-level
   container a function reads or a captured one, not only a frame local); ~~a
   DYN / unproven base~~ ✅ (the UNIVERSAL `StoreElemValue`: `vm_subscript_store`
   now handles a FLAT scalar base too via the shared `flat_store_core`, so it
   dispatches flat/general/dict at runtime like the tree-walker's
   try_flat->general — the codegen emits it as the catch-all for any
   container-slot base a fast path didn't take, preserving the unboxed
   StoreElemInt/Float for proven flat int/float arrays); ~~a `global = <expr
   with a call>` loop driver (bench 10)~~ ✅ (already 0-fallback: `r = f(x)` is a
   local store over a native CallV). The only remaining executed-code bench
   fallback is exceptions (42, P8).

5. **Part C - native-but-slow** (independent of fallbacks): computed-goto
   dispatch (C2, broad 10-20% on dispatch-bound loops — **and the DIRECT fix for
   the 2026-07-08 dispatch-slowdown regression**, since a bigger op set stops
   regressing the hot ops' branch prediction; pair with a cold-handler split),
   the builtin arg-view ABI (C3), `ModConst` (C4). The typed-read work (C1)
   largely landed as the typed dict / struct / element read ops.

**⛔ Negative result — foreach (F + D3), attempted P5 and REVERTED.** Built a
`ForeachBind` op that reuses a factored `foreach_bind_one` to make an array
foreach (indexed / general single-var; unpack excluded as unsound — its extra
vars can be `none` yet are typed non-null, which a native body would misread)
run native. It **regressed**: 19_foreach_indexed 0.27→0.30, geomean 4.02→4.01.
Root cause: the tree-walker's `do_iter` is ALREADY a tight C++ loop, and per
element the cost is dominated by `bind_loop_var` (the SAME in both engines) plus
the boxing in `arr_elem_boxed` — so the VM adds op-dispatch overhead without a
real native-body win (foreach bodies are small). The dict foreach (D3) is worse
(a bucket-walk / snapshot on top). **Lesson:** a fallback whose per-iteration
cost is a shared runtime helper (not `node->eval` of a rich body) is NOT worth
nativizing — the VM only wins where it removes real per-node dispatch. Left as a
tree-walker fallback (which is fast). Kept nothing; `git checkout` reverted the
op, the codegen, the inferencer flag, and the `do_iter` refactor.

## Current state (the foundations were then built; foreach re-done box-free)

The negative result above was the trigger for building the **AST-free
foundations FIRST** (see `bytecode-vm.md` + `vm-ast-free.md`): a **loc side
table** (`Chunk::locs`), a **const pool** + **member-key pool**, and a
**deferred backtrace loc** in `do_func_call`. With those, 8 op-data families are
now `node`-free (DictLoad, SubscriptV, boxed BinOpV/CompoundV/CmpV/LogV,
LoadGlobalV, MemberV, CallV/CachedCallV), and foreach was **re-done the RIGHT
way** — box-free, no `bind_loop_var`/`arr_elem_boxed`:
- **single-var general array** (`array<str|array|dict|dyn>`) → `LoadElemValue`
  binds the element's existing `EvalValue` (a copy, no box/unbox), ~1.7x on a
  general-foreach loop;
- **indexed 2-var** (`foreach i,e in indexed a`) → the index var IS the counter,
  element via `elem_th`;
- **closure/indirect call** (`c()`) → `CallValueV` (`11_closure_counter`
  1.09→1.00, `12_higher_order` 0.54x).

**Audit correction:** `-vd` shows only the TOP-LEVEL chunk, and a `func` decl
renders its body text inline — so an earlier per-bench audit over-counted "for
body" fallbacks that are really func-decl renderings (func bodies compile
separately via the Phase-4 hook). The genuinely remaining fallbacks are the
table rows above; the biggest real losses were all noise/parity once
`11_closure` was fixed.

**The one missing ABSTRACTION** is dict iteration (D3): a dict has no O(1)
index, so the counted-loop machine can't express `foreach k,v in d`. Build a
proper VM dict-iterator (iteration-state slot + a `DictIterNext` op), NOT the
cheap snapshot-to-two-arrays hack. Everything else is index-model ops.

**Language simplification is a lever (MyLang is pre-release).** We already used
it: **`foreach` array-destructuring is now STRICT** (ragged/scalar/mismatch
error instead of the old none-padding), which both improves semantics AND
unblocks the native unpack lowering (plain scalar reads, no per-element
is-array?/size? branch). And **`var` is now optional in `foreach`** (declares;
no-`var` may not shadow). Look for more such wins: a rarely-used dynamic
behavior that blocks nativization is a candidate to tighten (with a `README` +
`samples/` update — see CLAUDE.md).

**Order:** native unpack (F3), then the index-model ops (Lit / S / M),
then the dict-iterator foundation (D3), then func-body/free-the-AST, and
**exceptions (X) LAST** (needs VM-level exception dispatch to kill the C++
`throw` cost).

**Goal restated:** lift the `-vm` geomean from **~4.0× CPython** to **5×+**.
Progress (60-bench geomean; `run.py`'s 61-set reads a touch higher):
**baseline 3.76× → P1 (bool) 3.82× → P2 (dict store) 3.96× → P3 (dict read)
3.97× → P4 (general store) 4.02×.**

**2026-07-08 — a TRACKED geomean dip, ROOT-CAUSED.** The reported ~**4.5×** →
~**4.2×** `run.py --vm` drift was decomposed apples-to-apples (old binary vs
now, same bench set): ~HALF is a **bench-set artifact** (the recently-added
`63`/`64`/`66` benches are below-average — the OLD binary itself drops
4.36×→4.15× just from including them, no code change), and ~half is a small
**real ~4–5% regression** on dispatch-bound int/float benches (`01_while_loop`/
`03_int_arith`/`44_primes_sqrt`/`60_bit_sieve`) whose hot ops (`IntBin`/
`ForLoopStep`/`JumpUnlessIntCmp`/`LoadElemInt`/`StoreElemInt`) were untouched —
so it's the `vm_run_chunk` SWITCH growing (~8 new op cases → worse code-side
I-cache/branch prediction), NOT the Instr size (unchanged). Direct fix =
**computed-goto dispatch (C2)** + a cold-handler split; `drop-Instr::node`
(Step 5) is a secondary data-side win. Full data, the drift caveat, and the
profile/re-measure set in the "Perf regression to RECOVER" note in
`vm-ast-free.md`. Do NOT revert a nativization to chase it.

**Method:** ran `mylang -vd` on all 62 `bench/my/*.my` and recorded every
AST-fallback op (`eval.stmt` = `EvalStmt`, `eval.slot` = `EvalToSlot` — the two
ops that re-enter `node->eval`). Cross-referenced with the per-benchmark
VM/CPython ratio (`bench/run.py --vm`, release + `ASSERTS=0`). All numbers below
are that run (2026-07-05, HEAD `c05d6aa`).

## The geomean math (why one outlier is NOT the answer)

The geomean is a *product* over ~60 benchmarks, so a single benchmark moves it
by `ratio^(1/60)`. Concretely, measured:

- All 60 timed: geomean **0.266 (3.76×)** — `run.py`'s 61-set prints 4.0×; the
  difference is one near-zero const-fold bench it drops.
- **42_exceptions is 24.5× (VM *slower* than CPython)** — the worst by far — yet
  removing it only lifts the set to **4.06×**. Fixing it to 1.0× → **3.97×**;
  to 0.3× → **4.05×**. So exceptions is worth **~5%**, not the story.
- To reach **5× (geomean 0.20)** from 0.246 (the 59 non-exception benches) the
  product of all improvements must be `0.20/0.246 = 0.81` overall, i.e. roughly
  **double ~15 laggard benchmarks** (or 20 by 1.7×). This is a BROAD-category
  job, not one hero fix.

So the roadmap is organized by CATEGORY (how many benchmarks it moves), with
exceptions as a standalone (it's the vm-endgame "kill C++ exceptions" goal and a
real 24×, just not a geomean mover).

## Part A — the fallback inventory (what `-vd` found)

40 of 62 scripts are already 100% native. The 22 with fallbacks, grouped by the
underlying construct (● = hot / in the amplifier loop, ○ = cold one-time setup):

- **D1 · dict store** `d[k]=v`, `d.k=v` — benches 23, 24, 26, 27, 47, 62 (six).
  `StoreElem*` handle FLAT arrays only; a dict subscript store has no op → the
  whole build loop is `eval.stmt`. ●
- **D2 · dict read typed** `d.k`/`d[k]` on `dict<_,int/float>` — bench 25.
  `member.v`/`subscript.v` BOX even when inference proved the value is int;
  the tree-walker has `MemberExpr::eval_int` (`dict_present_value`) but the VM
  has no typed dict read. ●
- **D3 · dict foreach** `foreach(k,v in d)` — benches 26, 47, 62. Native foreach
  lowers only a flat `array<int/float>` single var; a dict foreach is
  `eval.stmt`. ●
- **B · bool array r/w** `a[i]=true`, `if(a[i])` — benches 43, 56, 57.
  `StoreElemInt/Float` + `LoadElemInt/Float` cover int/float; a flat
  `array<bool>` element has NO op, so the whole sieve is tree-walked. ●
- **F · foreach unpack/indexed** `foreach(x,y in a)`, `foreach(i,e in a)` —
  benches 19, 20. Native foreach = single non-indexed var; unpack/`indexed`
  fall back. ●
- **C · closure / indirect call** `c()` (a `FuncObject` in a slot) — bench 11.
  `CallV` fires only for a call devirtualized to a GLOBAL user-func slot; a call
  through a local/captured func value → whole loop `eval.stmt`. ●
- **X · exceptions** `try/catch` + `throw` — bench 42 (24.5×). try/catch is
  `eval.stmt`; every `throw` is a C++ `throw` (~1.6µs: heap + DWARF unwind).
  100k throws = the 24×. ●
- **S · string element** `s[i]`, build — benches 29, 30, 31, 32. `LoadElem*` are
  array-only; a string subscript is boxed `subscript.v`; `parts[i]=str(i)` (an
  array<str> store) is `eval.stmt`. ●
- **G · general/str array store** `a[i]=<non-scalar>` — benches 46, 20, 31, 32,
  47. `StoreElem*` store a scalar into a FLAT vector; storing an array/struct
  VALUE into a general array element → `eval.stmt`. ●
- **M · multi-assign / IdList** `a,b = ..`, `a,b,c = [..]` — benches 22, 06. An
  `IdList` assignment target has no native lowering. ● (22) / ○ (06)
- **R · call-in-global-assign** `r = f(n) + 900` — bench 10. The loop assign
  compiler doesn't handle `global = <call-containing expr>`, so a recursion-
  driving loop is `eval.stmt` (the recursion is tree-walked). ●
- **A · append value self-eval** `push(a,i)` — bench 13. `CallBuiltinLV` fires
  (native dispatch) but `push` self-evals its VALUE arg (a `node->eval` of `i`).
  ●
- **cold-only (ignore)** — most `a=[..]`, `d={}`, `func f(){}`, `sort`/`map`/
  `filter` decls and one-shots: a one-time `eval.stmt`; the heavy builtin
  dominates its own dispatch. ○

## Part B — prioritized fix roadmap

Ordered by (impact × breadth) ÷ risk. Each step: one op or lowering, gated on
the 1156-test differential + RECYCLE+ASan, benched `--vm` before/after.

### P1. Bool array element read/write — `StoreElemBool` / `LoadElemBool`
*Benches: 43_sieve (1.08→~0.4), 56_sieve_bool, 57_bool_reduce. LOW risk.*
Mirror the existing `StoreElemInt`/`LoadElemInt` exactly, over `bvec` (the
`unsigned char` flat store) with the same bounds/negative-wrap/COW/hash-
invalidate. `a[i]=true/false` and `if(a[i])` in the sieve become native (the
inner `while(j<n){primes[j]=false;j+=i;}` is ALREADY native but for that one
store). Highest value-to-effort ratio; **do first**.

### P2. Dict subscript store — `DictStore` — DONE (2026-07-05)
Landed: a `base_dict` inferencer flag (mirrors `base_array`) + a `DictStore` op.
Codegen recognizes `d[k] = v` / `d[k] OP= v` with a local-slot dict base,
compiles the value then the key to boxed temps (rhs-then-lvalue order), emits
`DictStore`. `vm_dict_store` (eval.cpp) reuses the shared
`Type::subscript(for_write)` lvalue path (auto-vivify on a plain-assign miss,
COW, container-key freeze) + `slot_rmw` (assign/compound), so results, the
missing-key-on-compound `KeyNotFoundEx` (with the SUBSCRIPT's caret), read-only-
dict `NotLValueEx`, and default-dict vivify match the tree-walker. A non-dict
runtime base (none / dyn-laundered) falls back. 1306→ +2 tests, 1157/1157
differential, RECYCLE+ASan. Benches: 23 .72→.55, 62 .67→.43, 26 .39→.32, 27/47
.7→.67; geomean 3.82×→3.96×. **Follow-ups:** a MEMBER store `d.k = v` (D1m/P2b,
rare) and a GLOBAL/capture dict base (only local slots for now) stay fallback.

*(original design)* A dict subscript/member store, biggest category. A
`d[k]=v` / `d.k=v` op: read the dict slot, evaluate key+value operands (boxed),
call the shared `TypeDict::subscript(..,for_write=true)` path (auto-vivify,
key-freeze, COW) the tree-walker uses — a runtime FUNCTION, no `node->eval`.
Compound `d[k]+=v` too. This un-flattens the dict-build loops gating 6 benches.

### P3. Typed dict READ — `DictLoadInt` / `DictLoadFloat` — DONE (2026-07-05)
Landed: a `base_dict` flag on MemberExpr (mirrors the Subscript one) + a
`DictLoadInt`/`DictLoadFloat` op pair. `compile_int_expr`/`compile_float_expr`
recognize a th-int/float dict `d.k` (member) / `d[k]` (subscript) read with a
local-slot dict base and emit the op into a temp (a subscript's key is a boxed
temp in `a`, a member's is the node's `memId`, distinguished by
`node->is_subscript`). The handler reads the present-key scalar directly via the
now-shared `dict_present_value` (the SAME map find the tree-walker's `eval_int`/
`eval_float` uses; a bool value → 0/1, int→float promotion for the float
variant); a MISSING key / non-dict base falls back to `node->eval_int`/
`eval_float` (default-dict / `KeyNotFoundEx`), like LoadElemInt. Because
`compile_boxed_expr` delegates a th-scalar node to compile_int/float, even a
boxed-context read (`var dyn x = d["a"]`) now takes the faster typed path (shape
test #24/#25 updated). Verified 1309/1309 + 1160/1160, RECYCLE+ASan. Benches:
25_dict_member .75→.53, 24_dict_lookup .27→.17; geomean 3.96×→3.97×.
**Follow-up:** a typed STRUCT member read `s.x` still boxes (a separate op).

*(original design)* When inference proved `dict<_,int/float>`, read the

### P4. General array element store — `StoreElemValue` — DONE (2026-07-05)
Landed: a `StoreElemValue` op for `a[i] = v` / `a[i] OP= v` where the array is
`base_array` but the element is NOT a flat scalar (`th != i/f`) - an
array/str/struct/dyn element. Same codegen shape as DictStore (local-slot base,
value then index to boxed temps). The `vm_dict_store` helper was generalized +
renamed **`vm_subscript_store`** (it was already type-dispatched via
`Type::subscript(for_write)`), so the same function now backs DictStore (P2) AND
StoreElemValue (P4) - the array's bounds check + COW (alias vs slice-clone)
and the store come straight from the shared runtime, matching the tree-walker.
A non-array runtime base falls back. Verified 1310/1310 + 1161/1161 (+1 test:
nested/str/compound/alias-share/slice-COW), RECYCLE+ASan. Benches:
32_str_build .82→.67, 20/47 .66/.65→.62, matrix flat; geomean 3.97×→**4.02×**
(crossed 4× on the 60-set). **Follow-up:** a nested base `a[i][j] = v` (general)
and a global/capture base stay fallback.

*(original design)* `a[i]=<value>` where the element is a non-scalar

### P5. foreach — unpack, indexed, and dict
*Benches: 19, 20 (.69), 26, 47, 62. MED/HARD.* Extend the native-foreach
lowering: (a) tuple unpack `foreach(x,y in a)` over a flat/general array;
(b) `indexed`/`foreach(i,e in a)`; (c) dict `foreach(k,v in d)` (snapshot the
bucket walk). (c) is the hard one — a dict has no flat index; lower to an
iterator cursor op. Big for the dict-heavy benches (overlaps D3).

### P6. Indirect / closure calls — `CallValue`
*Bench: 11 (.60). MED.* A call whose callee is a `FuncObject` VALUE in a slot
(local/captured/global-var, not a devirtualized global-func slot): read the
slot, check `is<FuncObject>`, evaluate args into a register run, `vm_call_func`.
Generalizes `CallV`, so `s += c()` loops go native. (The closure `start++`
on a captured var is a separate small fallback — a capture-slot store.)

### P7. String element read — `LoadStrChar` / typed
*Benches: 29, 30 (.44), 31. MED.* `s[i]` → the char (or, for `ord(s[i])`
loops, fuse to an int char code). String build (`parts[i]=str(i)` then `join`)
overlaps P4.

### P8. VM-level exceptions — no C++ throw for user `throw`
*Bench: 42 (24.5×). HARD, high-symbolic-value (the vm-endgame goal).* Lower
`try/catch/throw/rethrow` to VM control flow: a `try` pushes a handler record
(catch-label + catch-var slot + finally-label) onto a VM handler stack; `throw`
of a struct sets a "pending exception" and jumps to the top handler's catch
dispatch (match by struct name) WITHOUT a C++ `throw`; `finally` runs on the
unwind path. Only RUNTIME-error exceptions (div-by-zero) still use C++ throw
(rare). Removes ~100k C++ throws in the bench → 24.5× should collapse toward
~1×. It's ONE bench for the geomean (~5%) but the single most-dramatic per-bench
win and the stated endgame direction — schedule after the broad wins.

### P9. Multi-assign / `IdList` — native
*Benches: 22 (.64), 06.* Lower `a,b = expr` / `a,b = [..]` to the element-spread
the tree-walker's `handle_single_expr14` does, over slots.

### P10. `push`/`append` plain-value ABI + call-in-assign
*Benches: 13 (.79), 10 (.64).* (a) give `push`/`append` a value-ABI plain-append
path so the value arg isn't self-eval'd (2b's emplace already killed the ctor
case). (b) Extend the loop-body assign compiler to accept `global = <expr with a
CallV>` so 10_recursion_deep's driver loop goes native (the recursion stays a
real call, but stops being tree-walked wholesale).

## Part C — VM optimizations (native but sub-optimal)

Independent of fallbacks — these speed code that is ALREADY native:

1. **Typed operands into member/subscript/builtin reads.** P3 is the biggest
   instance: many `member.v`/`subscript.v`/`call.blt.v` feed a typed arith chain
   but box. A typed-read variant (int/float dst) removes a box+unbox per
   hot read. 40_math_builtins (.42, 0 fallbacks) is gated by boxed builtin
   results feeding typed adds.
2. **Dispatch via computed-goto** (`&&label` threading) instead of the `switch`
   in `vm_run_chunk`, on GCC/clang. Removes the range-check + the single
   indirect-branch misprediction hub; classic 10–20% on a dispatch-bound loop.
   Keep the `switch` fallback for MSVC.
3. **`CallV`/`CallBuiltinV` arg movement.** Args are copied slot→buffer→callee.
   Where the callee reads a contiguous frame run, pass the run by pointer (the
   `VmArgs` view already does this for user calls; extend to the builtin ABI so
   `call.blt.v` doesn't `EvalValue`-copy each arg).
4. **Fuse `x = x % C`** (a constant modulo in nearly every bench's `s = s %
   1000000007`). Already native (`IntBin`), but a `ModConst` immediate-second-
   operand form saves the operand decode. Minor.
5. **Avoid re-boxing a bool comparison result** that immediately feeds a branch
   (largely done via `JumpUnlessIntCmp`; audit `bin.v`+`JumpIfFalse` pairs for a
   fused `cmp.v`+branch).

## Suggested execution order

P1 (bool arrays) → P2 (dict store) → P3 (typed dict read) → C1/C2 (typed reads +
computed goto, broad) → P4 (general store) → P5 (foreach) → P6 (closures) → P7
(strings) → P9/P10 (multi-assign, append) → P8 (VM exceptions, last — hardest,
one bench). Re-bench `--vm` after each; expect the dict + bool + typed-read
cluster (P1–P4, C1) to do most of the 4.0→5× lift since it moves ~12 benchmarks.

Each step is small, differential-gated, RECYCLE+ASan-clean, and doc-synced in
the same commit — same cadence as Phase 2a/b/c.
