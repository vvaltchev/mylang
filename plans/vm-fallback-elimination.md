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

## ⚑ FALLBACK-OP AUDIT (2026-07-10, VERIFIED) — exceptions were NOT the last

P8 exceptions were a big fallback category and are gone, but they were **not**
the last. A fresh `-vd` sweep over every `bench/my/*.my` AND `samples/*`, with
**dead template chunks excluded** (a `func f` whose calls all redirect to a
native `f$0` is never compiled at runtime; its `eval.stmt` rows are noise - the
`f$0` instance is native), finds these LIVE fallback ops. All are `EvalStmt`
(no live `EvalToSlot` or `JumpIfFalse` remain - every condition/expression the
samples use is native). Goal: nativize each → `node` becomes unused → drop it.

- **F-1 · multi-assign / IdList** (three forms) — ✅ DONE (2026-07-10).
  A catch-all `MultiUnpackV` op (codegen `try_multi_unpack`) now covers the
  whole IdList branch after the two array-eliding fast paths: it compiles the
  rvalue into a temp and the op does the tree-walker's STRICT destructure
  (an ARRAY value → length-checked distribute via `vm_arr_elem`, which
  dispatches on `skind()` so it is sound for a general/flat/dyn element; a
  NON-array → spread to every target). AST-free: the target slots live in
  `Chunk::unpack_targets` (`-1` == `_`), the strict-length caret in the loc side
  table (recording the enclosing `Expr14`'s span, matching the tree-walker's
  `Construct::eval` stamp of a loc-less IdList). Bench `73_multi_unpack` (the
  array-VALUE form) VM 0.30x vs the tree-walker; `22_multi_assign` (literal,
  elided) 0.19x. All three original forms verified native:
  - **F-1a** `var a, b, c = [lit]` (`fib`) — a const array literal folds to a
    `LiteralObj`, so it builds via `LoadLiteralObjV` then `MultiUnpackV` (the
    non-const-literal ASSIGN form still elides via `try_multi_literal_store`).
  - **F-1b** `for (var n, tot = [lit]; …)` (`primes`) — native.
  - **F-1c** `a, b = <array VALUE>` (`shopping`: `= products[pnum]`) — native,
    the strict-unpack-a-value op this needed.
- **F-2 · foreach** (two shapes):
  - **F-2a** `foreach (var k, v in data)` - dict 2-var (`phonebook`). ✅ DONE
    (2026-07-11). DIAGNOSED: a proven-dict 2-var foreach was ALREADY native
    (DictIter*); the blocker was that `cmd_view` is dispatched INDIRECTLY (via
    the `cmdfunc` func-value, F-3), so its `data` param is never concretely
    instantiated and stays `dyn` → the 2-var DYN-container foreach fell back
    (ForeachDyn was single-var only). FIX: extend `ForeachDynInit`/`Next` +
    `DynIterState` to a var count (1 or 2); a 2-var dict binds key+value, a
    2-var array element is STRICT-unpacked (do_iter's messages/caret). The
    inferencer now stamps `container_is_dyn` for a 1- OR 2-var non-indexed dyn
    foreach. Bench `74_dyn_foreach_kv` VM 0.54x vs the tree-walker; phonebook
    `cmd_view` verified native + end-to-end parity.
  - **F-2b** `foreach (i, name, price in indexed products)` - indexed + 2-elem
    unpack (`shopping`). ✅ DONE (2026-07-11). Added a general-value unpack op
    `UnpackElemValue` (the analogue of `UnpackElemInt/Float` for a general / dyn
    / str / mixed sub-array like shopping's `[str, float]`), and extended
    `try_native_foreach_unpack` + the inferencer stamp to cover the `indexed`
    form (index var = the loop counter, the unpack targets follow it). Also
    FIXED a latent front-end bug: `accumulate_foreach` typed an indexed 3+var
    loop's targets as the whole sub-array (`array<dyn>`) instead of its element
    type (`dyn`/`str`) - shopping survived only via lenient builtins, never
    arithmetic. shopping is now ZERO-fallback (both F-1c and F-2b gone). Bench
    `75_indexed_unpack` VM 0.71x vs the tree-walker.
  - **F-2 BUG (found + FIXED 2026-07-10, pre-existing)** — `UnpackElemInt`/
    `UnpackElemFloat` (the already-native `foreach (a, b in pairs)` strict
    unpack) blindly read the sub-array via `flat_ints()`/`flat_floats()`, but a
    MIXED-numeric sub-array literal `[int, float]` is typed `array<float>` by
    the inferencer (int∨float join) yet built GENERAL storage at runtime → the
    flat accessor ASSERTED / crashed (`shopping`: `[pnum, q]`; minimal repro:
    `append(lst,[i,f]); foreach(a,b in lst){}`). The differential was green only
    because no `-rt` test used a mixed sub-array. FIX: the op now guards the raw
    read on `sub.skind()` == the expected flat kind; else it binds each
    element's actual boxed value via `vm_arr_elem` (skind-dispatched) — box-free
    still for the common homogeneous-flat case, sound for the general/mixed
    case. Regression test + shopping parity added.
  - **F-2c** `foreach (a, _, c in pairs)` — a `_` PLACEHOLDER or a
    non-consecutive loop-var layout. ✅ DONE (2026-07-13). The existing
    `UnpackElem{Int,Float,Value}` ops write a CONTIGUOUS slot run, but a `_`
    gets no slot (resolver skips it) and can sit between real targets, so the
    run isn't contiguous → the old codegen fell back. Added `UnpackElemTargets`:
    same op shape, but a per-position target-slot list lives in the
    `Chunk::unpack_targets` pool (`-1` == `_`, skipped) and each element binds
    box-free via `vm_arr_elem` (flat or general — one op for int/float/str/dyn
    sub-arrays). `try_native_foreach_unpack` builds the targets vector, detects
    `_`/non-consecutive, and emits the tuned contiguous op OR the targets op;
    covers the non-indexed and indexed forms. node-free (loc side table). Tests
    added for leading-`_` general + middle-`_` flat-int.
- **F-3 · indirect call statement** `cmdfunc(data)` - a discarded-result call
  through a func-VALUE var (`phonebook`). ✅ DONE (2026-07-11). DIAGNOSED: the
  EXPRESSION-position value call was already native (`CallValueV`); the gap was
  that a discarded call STATEMENT had no handler for a plain `CallExpr` (only
  `DirectCallExpr`/`DirectBuiltinCallExpr`), so it fell to `EvalStmt`. FIX: both
  `gen_stmt` and the loop-body stmt compiler now route a plain-`CallExpr`
  statement to `try_native_value_call` (which rejects a `Direct*` internally),
  discarding the result. (The `opt func` narrowing was a red herring - the
  callee's kind is `Func` even when `opt`, so `vm_direct_func` was already set.)
  phonebook's `cmdfunc(data)` is now `call.val`; the only remaining `EvalStmt`
  is the DEAD `load_data` base template (its live `load_data$0` instance is
  native). Bench `76_funcval_dispatch` VM 0.93x vs the tree-walker (call-bound,
  so modest; the win is removing the dispatch overhead + the fallback).
- **F-4 · flat `array<PodStruct>` literal** `[P(a), P(b)]` (synth). ✅ DONE
  (2026-07-11). Removed the `MakeArrayV` `flat_s` bail: each `P(..)` element
  already lowers to a `StructCtorV` (a POD `StructObject`), and
  `build_array_from_values` packs a run of same-type POD structs into flat
  mode-5 storage VALUE-DRIVEN (the def comes off the first element, so the op
  needs no def). Also relaxed the `StructCtorV` arg gate to accept a scalar
  LITERAL (not only a `th`-stamped operand) so an AUTO-CONST-folded arg
  (`var a=1; [P(a,a)]`, whose folded literal has no `th`) lowers too.
  **Regression ERASED (2026-07-11) by the fused `MakeStructArrayV`:** the
  initial lowering (per-element `StructCtorV`×N + `MakeArrayV`) was VM ~1.2x
  SLOWER than the tree-walker's already-optimal single-dispatch
  `LiteralArray::do_eval` (splitting one dispatch into N+1 ops + a temp-slot
  round-trip). The fused op (`try_make_struct_array` -> `MakeStructArrayV` ->
  `vm_make_struct_array`) compiles the N structs' field args INTERLEAVED into
  one run and coerces them STRAIGHT into the flat byte buffer - NO intermediate
  `StructObject` per element (the `EmplaceStruct` pattern for a whole literal).
  It now BEATS the tree-walker: `77_struct_array_lit` VM **0.85x** (1.2x FASTER)
  / 0.70x CPython. A mixed / nested-struct-field / non-scalar-arg literal
  declines the fused op and falls to the per-element path, then `EvalStmt`.
- **F-5 · reflection builtins** — split by what they actually need (diagnosed
  2026-07-11; the earlier "one INHERENTLY-node case" was too pessimistic):
  - **`show()` — DONE, made a DEV-ONLY builtin.** It decompiles the AST, so it
    genuinely needs the node - but a compiled SCRIPT doesn't retain the AST, so
    rather than force the AST into script bytecode, `show()` is now reserved to
    the DEV harnesses (REPL + tests, which keep the AST). `make_dev_builtin`
    (`types.cpp`) registers it + records the name in `g_dev_builtin_ids`;
    `is_dev_builtin` / `g_dev_builtins_allowed` (`eval.h`); the inferencer's
    `reject_dev_builtins` (structural pass, fires even under -nti) makes a script
    call a compile-time `SyntaxErrorEx`, while the REPL / `-rt` runner set the
    flag so it works there. So `show` NEVER reaches serialized bytecode - the
    `.myv` goal is unblocked without nativizing it. (`:show` is a REPL
    meta-command, never a builtin, unchanged.)
  - **`type`/`typestr`/`kindstr`/`decltype` — ✅ DONE (2026-07-11).** (1) The
    inferencer's `fold_type_query` sets `CallExpr::tq_folded` when it bakes the
    answer into `args[0]`; both engines then ELIDE the folded call (return the
    baked literal - the VM a `LoadConstV`/`LoadLiteralObjV`, the tree-walker in
    `do_eval`), so the common case is a plain constant, no call, AST-free +
    faster. (2) The rare non-folded query (`-nti` / `Unknown` arg) is a dual-ABI
    builtin (`make_builtin_customv`): a custom `func` + a `func_v` (the VM's
    `CallBuiltinV`), BOTH always building the `Type`/string from the runtime
    value. The flag (not a node `dynamic_cast<Literal>` check) is what keeps it
    `-nti`-correct AND fixed a latent tree-walker bug (`typestr("hi")` under
    `-nti` now reports `"str"`, not `"hi"`). No new storage. So the ONLY
    node-holding reflection residual is the dev-only `show` (deliberate).

**`node`-field status: ✅ DROPPED (2026-07-11).** `Instr` no longer holds a raw
`Construct*` - the field is a 4-byte **`node_idx`** into a new **`Chunk::ast_nodes`**
pool (`std::vector<const Construct*>`), so the instruction stream has no AST
pointer to serialize. `Codegen::add_ast_node` pools during codegen; `extract_locs`
nulls the loc-only ops (their caret is in the loc side table) and a KEEP-list
marks the genuine runtime-node ops (the fallbacks, the builtin-call ops for their
args `ExprList`, the flat int/float store's OOB/div0 caret), `default` nulling
the rest; `compact_ast_nodes` then rebuilds the pool with only the live entries.
So a **fully-native chunk ends with an EMPTY pool** - a non-empty `ast_nodes` is
EXACTLY the "this chunk still needs the AST" signal (`-vd` dumps it *NOT
serializable*). This is the ONE non-serializable pool left; everything else
(`locs` / `member_keys` / `catch_types` / `literal_objs` / `closure_defs` /
`struct_defs` / `unpack_targets`) is plain data or by-name re-internable. So the
`.myv` writer's rule is simple: a chunk with an empty `ast_nodes` serializes; a
non-empty one keeps its AST (a fallback / a dev-only `show` / an unmigrated
caret). Remaining to a 100%-serializable image: migrate the flat-store caret to
the loc table (removes `StoreElemInt`/`Float` from the pool), and the builtin ops
to a by-name + arg-loc encoding (removes their `ExprList` need) - then only true
`EvalStmt` fallbacks + dev-`show` keep a node.

**AST-FREE BUILTIN + STORE MIGRATION: ✅ mostly DONE (2026-07-12).** The value-ABI
path and two more op families are now AST-free, so a builtin-and-array-heavy
native chunk (e.g. `for(i;i<len(a);i++) a[i]=i*i; print(a)`) ends with an EMPTY
`ast_nodes`:
- **`func_v` no longer names the AST.** Its signature took an `ExprList *` purely
  for carets + the repr hint; it now takes an AST-free **`ArgLocs`** (evalvalue.h)
  — the whole-args + per-arg carets + `arr_hint`. The tree-walker adapter builds
  it from the real `ExprList` (`build_arglocs`), the VM from a pool
  (`vm_build_arglocs`→pool). All ~68 `func_v` builtins migrated
  (`elems[i].get()`→`arg(i)`); the lvalue-ABI (`func_lv`) + AST builtins keep
  `ExprList *`. (This DID touch the builtin sigs — cleaner than the earlier
  "loc-handle only" plan, and it makes the value-ABI itself node-free.)
- **`CallBuiltinV` → the serializable `Chunk::builtin_calls` pool** (`target2`
  indexes `{Builtin, name, carets, arr_hint, per-arg carets}`); the pooled `name`
  (`UniqueId*`) is what a `.myv` writer re-binds the func ptr from. No `node`; the
  hot path is leaner (per-arg loc copy moved to codegen). `-vd` renders
  `call.blt.v r4 = print(r3)` + a `builtin_calls` dump.
- **`CheckFuncV` / `MapFilterV` → the loc side table** (single-caret ops, like
  SubscriptV/MemberV).
- **flat `StoreElemInt` / `StoreElemFloat` → node-free.** The `node->eval`
  fallback (const/read-only/general/dyn/bool-compound) now routes to the UNIVERSAL
  `vm_subscript_store` (box the already-computed index/value operands, map the
  base op → Expr14 op via `vm_base_to_expr14_op`) — the same differential-proven
  store StoreElemValue/DictStore use; the OOB/div0 caret moved to the loc table.
  (The tree-walker's flat OOB uses the narrower *subscript* loc while the VM
  records the *statement* loc — a pre-existing single-loc divergence, unchanged.)

**The MUTATING-BUILTIN node-drop: ✅ DONE (2026-07-12)** except EmplaceStruct.
`CallBuiltinLV` / `CallBuiltinLVElem` are now node-free (pooled in
`builtin_calls`, like CallBuiltinV). It landed as the scoped multi-commit effort
(each `-rt` 1425/1425 + 1270/1270 + samples byte-identical):
  1. **PER-OP rest-native mechanism** (`lvalue_rest_capable`): the rest-native
     decision moved from the overloaded per-builtin `lvalue_rest_native` to a
     PER-OP one - the VM reads `in.b.is_lit` (a compiled rest run), the codegen
     decides per call site. So append/push's THREE shapes coexist: plain value =
     rest-native, ctor = `EmplaceStruct`, subscript = `CallBuiltinLVElem` (the
     last two gated on the ctor-shape / subscript-base checks, NOT the flag).
  2. **append/push construct-in-place → a custom tree-walker `func`** (`append_tw`
     via `make_builtin_lv_custom`); `builtin_append` (func_lv) is rest-native-only.
  3. **sort/rev_sort cmp → rest-native** (`sort_arr` pre-evals it, `sort_core`
     uses `rest[0]`); reverse has no value args. So NO func_lv self-evals.
  4. **LVElem rest-native** (compiles `[index, values]` into one run) + a rest-
     capable ctor/no-lower **→ EvalToSlot** (never a self-eval CallBuiltinLV).
  5. **func_lv → the AST-free `ArgLocs` ABI** (parallel to func_v; ArgLocs gained
     `nargs` for a self-eval builtin's arity check), then **pooled** into
     `builtin_calls` (index in `a.slot`; `a.lit` = the arg0/base slot kind).
  Added subscript-target (`append(a[i],x)`) test coverage (was an untested gap).
  **REMAINING: `EmplaceStruct`** - the one builtin op still node-holding (it needs
  the ctor's `vm_struct_ctor_def` + field-arg carets); a separate pooling
  (`struct_defs` + a field-caret pool).
- The **fallback ops**: was `EvalStmt` / `EvalToSlot` / `JumpIfFalse`
  re-entering `node->eval`. **DIRECTIVE (2026-07-12): 100% of them MUST be
  removed.** ✅ **DONE (2026-07-14): `EvalToSlot` + `JumpIfFalse` are DELETED**
  (see the Tier-1 endgame audit below); **`EvalStmt` remains as THE single
  fallback op** — whole-statement granularity only, reachable only by
  show()-in-tests + the R4 value form. Historical per-op notes (how each
  became unreachable):
  - `JumpIfFalse` — **DONE (a `!x` condition is now native).** A unary op over a
    dyn/general operand (`!x`, `-x`, `~x`, `+x`) now lowers to a boxed
    **`UnaryV`** op (`compile_boxed_expr` handles `Expr02`), so `if (!x)` is
    `unary.v r = !x` + `jmp.ifnot.v` (native) and `var b = !x` is `unary.v` +
    `move` — no `JumpIfFalse` / `EvalToSlot`. Verified: no `!x` condition emits
    `JumpIfFalse` in any bench/sample; `-str`/`~str` type-error carets +
    backtraces are byte-identical between engines. The `JumpIfFalse` op still
    has OTHER emitters (a fallback body drags its loop's condition to the
    fallback form — e.g. `array<bool>` element ops in the sieve, a `for` with a
    `return` in its body); those are the array/loop nativization items, not a
    `!x` gap. **(Superseded: JumpIfFalse is now DELETED — an if/while whose
    condition can't lower falls back WHOLE-statement via EvalStmt.)**
  - `EvalToSlot` — an AST builtin (`defined`/`isconst`/`type`/`decltype`/
    `typestr`/`kindstr`) in a scalar-expression position. These are COMPILE-TIME
    ONLY: with full AOT inference they fold to a literal (a script is a closed
    world — `defined(name)` is knowable, `type(x)` is the inferred static type),
    so they never reach codegen. **`defined` FOLD GAP CLOSED** (`try_fold_defined`,
    resolver): `defined(name)` now folds to `true` when `name` resolves to a
    LOCAL (param bound at every call; a resolved local is reached only after its
    decl ran — no per-slot liveness), a CAPTURE (snapshot at creation), or an
    unshadowed BUILTIN — all always-bound, sound in script + REPL with no gating.
    **The one legitimately-non-foldable case is `defined(GLOBAL)`** — a global's
    slot has a runtime `defined`-flag set only when its decl executes, so
    `defined(g)` is genuinely `false` before / `true` after that runs (a real
    runtime property, NOT a fold gap). **DONE — `defined(global)` now lowers to a
    native `DefinedGlobalV`** op (`try_native_defined_global`, codegen) that
    reads `gfuncs->defined[slot]` as a bool (the slot is known at codegen —
    AST-free, `node_idx` stays -1, never throws). `type`/`decltype`/`typestr`/
    `kindstr` fold via `fold_type_query` (or, non-folded, dispatch as a dual-ABI
    `CallBuiltinV`, never EvalToSlot); `isconst`/`isconstdecl` fold via
    `fold_isconst` (always). **So in a SCRIPT every AST-builtin path is folded or
    native — EvalToSlot is unreachable.** The ONE residual emitter is the
    **dev-only `show`** (an AST decompiler with no value ABI), which is a
    compile-time error in a script (so never in `.myv`) and only reachable in the
    REPL / test harness. **(Superseded: EvalToSlot is now DELETED — `show`
    in a scalar position fails its expression and the whole containing
    STATEMENT falls to EvalStmt, which the -rt differential accepts.)**
  - `EvalStmt` — the general statement fallback. After AOT + full `dyn` (below),
    the only bodies that reached it (untyped template bases) no longer exist as
    chunks, so it too is unreachable.
  After each is proven unemitted (an emitter audit), DELETE the opcode + abort-
  guard any residual with an `ML_CHECK(false)`-style assert (NOT
  `__builtin_unreachable` — that's UB if reached; we want a loud abort).

**THE AOT / ZERO-FALLBACK ENDGAME (maintainer directive, 2026-07-12).** The VM
must be a fully AOT, zero-fallback engine, everything decided upfront by type
inference before any bytecode runs. Good news: the VM's **boxed value tier
already gives full native `dyn`** — `func foo(dyn x){ var dyn s=x; for(..)
s=s+x; return s; }` compiles to a fully-native chunk (`bin.v s = s + x ; boxed`,
`for.step`, no fallback). So the hard runtime part is done; the gaps are
front-end + compilation-model:
1. **AOT chunk compilation (NO lazy). DONE.** `vm_precompile_all` (vm.cpp),
   called at the top of `vm_execute` (AST final), walks `collect_funcs` and
   compiles EVERY function body upfront — stamping each `FuncDeclStmt::vm_chunk`
   + `vm_chunk_tried=true`, so `do_func_call` reads a precomputed pointer and
   NEVER lazily compiles (the lazy `vm_func_chunk` miss path is now only a
   never-hit safety net). The compile + gate is the shared `codegen_func_body`
   (codegen.cpp), the single source of truth for "which functions have
   bytecode", used by BOTH the precompile and `-vd` — so `-vd` drives off the
   real compiled chunk set. **Dead base templates are ABSENT** (not filtered):
   the inferencer marks `FuncDeclStmt::is_template_base` for a template that is
   NEVER used as a value (`!value_used`) — then all its calls are direct and
   were redirected to `name$N` instances, so the base never runs → no chunk →
   absent from `-vd`. A VALUE-used template (dict/var/arg-dispatched INDIRECTLY,
   e.g. phonebook's `cmd_*`, `76`'s `add_op`) is NOT marked — its indirect call
   runs the base body, so it keeps its chunk (compiled + shown), no regression.
   Verified: the only `-vd` change vs before is dead base templates (fib/gcd/
   is_prime/…, and a fully-const-folded `pure func heavy`) vanishing; every
   live instance + indirectly-dispatched base remains; suite + differential +
   all sample/bench outputs byte-identical. (The residual `!value_used`-but-
   still-runs cases — a D4 overflow >64, an uninstantiable direct call — just
   tree-walk instead of VM-run; first-class `foo$dyn` (item 2) erases them.)
2. **First-class `dyn` INSTANCE, decided upfront. DONE (already worked).** A
   template `foo(x)` called with a `dyn` arg already mints a native `foo$dyn(dyn
   x)` (the boxed tier: `bin.v ; boxed`), coexisting with `foo$int` — verified
   `-vd`/`:show`. An INDIRECTLY-dispatched template (dict/var, `76`'s `add_op`,
   phonebook `cmd_*`) runs its base body compiled NATIVELY (the boxed tier, no
   fallback) — see step (c)'s value-used-keeps-the-chunk. So dyn dispatch is
   already fallback-free. (Residual: a D4 overflow >64 still tree-walks the base
   — rare, a tracked follow-up.)
3. **`dyn` SEMANTIC — dyn-into-concrete COERCION (NOT `int OP dyn`->int).**
   DONE. The maintainer's ruling on `foo(x){ var s=0; s=s+x; }`: `var s` is
   `int`, and `s = s + x` works iff x's runtime value is int, else a runtime
   error.
   The RIGHT framing (a first `int OP dyn`->int attempt was WRONG — it made
   `var dyn r = 3 + d` wrongly throw, forcing `3+d` to int even in a dyn
   context; reverted): **`int OP dyn` is `dyn`** (the natural result). `s` keeps
   int because a `dyn` value is **assignable to a concrete NUMERIC local** — a
   runtime-checked coercion:
   - **Inference (`contribute`/`commit_round`):** a `dyn` contribution to a
     plain `var` is RECORDED (`round_got_dyn`) but NOT joined; the accumulator
     collects only non-dyn contributions. At commit: a NUMERIC accumulator + a
     dyn contribution → keep numeric, set `coerces_dyn` (sticky); a
     non-numeric / dyn-only var → fold the dyn back in → `dyn` (DynRequiredEx).
     So
     `var s = 0; s = s + x` → int; `var r = 3 + d` → dyn (declare it).
   - **Runtime:** the inferencer stamps the coerces_dyn var's decl
     `Identifier::decl_type` (i/f), so `resolve_names` propagates it and the
     store's `coerce_to_decl_type` fires — exactly like an explicit `int s`.
     `coerce_to_decl_type` is now STRICT: it widens (int/bool->float, bool->int)
     but NEVER narrows — a `float` into an `int` THROWS (use `int(x)`); `none`
     passes through. Both engines share the store path, so the differential
     covers the VM. NO new op, NO binop change, NO specializer change.
   - **A base template's body is NOT specialized** (`FuncDeclStmt::is_template`,
     skipped in `specialize_types`): a monomorphization shell, cloned per
     signature (each clone specializes separately) — specializing it would
     corrupt a different-signature clone (a float instance's `eval_int` on a
     float param). `type_of` learned the `TypedScalarExpr` case (a defensive
     robustness fix). Both kept from the earlier attempt.
   - **Known pre-existing limitation** (unchanged by this work): the inliner
     drops a typed PARAM's coercion when it splices the body (a widening
     `f(float x); f(runtime(3))` inlined keeps the int too), so an inlined
     typed-param call with a dyn arg does not run the strict coercion — a
     separate inliner fix.
4. **Base templates GENUINELY DON'T EXIST as chunks — NOT hidden in `-vd`.**
   With (1)+(2), every call targets an instance (typed/dyn); the base template
   is never called → never AOT-compiled → not in the bytecode image. `-vd` must
   be a FAITHFUL representation of what EXISTS (the compiled chunk set = the
   instances), so it shows no base because there IS none — NOT because we
   filter it out. (Drive `-vd` off the AOT-compiled chunk set, not a raw AST
   walk.) The maintainer explicitly rejects hiding: a faithful dump, no
   special-casing.
5. **AST builtins fold away (see the EvalToSlot bullet above)** — they are
   compile-time-only and must be literals before codegen under full AOT.
Order: (a) `!x` nativization **[DONE — boxed `UnaryV`]**; (b) close the
AST-builtin fold gap → EvalToSlot unreachable **[DONE in SCRIPT — `defined`
folds (bound) + `DefinedGlobalV` (global); all other AST builtins fold/dual-ABI;
only dev-only `show` still emits EvalToSlot, never in a script/.myv]**;
(c) AOT chunk compilation (all upfront, `-vd` off the chunk set) **[DONE —
`vm_precompile_all` + shared `codegen_func_body`; dead base templates absent via
`is_template_base` on `!value_used` templates]**;
(d) first-class `dyn` instances + the `int OP dyn`→int inference rule **[DONE —
dyn instances already native; the dyn accumulator via dyn-into-concrete COERCION
(`coerces_dyn` + strict `coerce_to_decl_type`), NOT `int OP dyn`->int (a first
attempt, reverted); D4-overflow foo$dyn deferred]**;
(e) audit each fallback op is unemitted, then DELETE + abort-guard **[AUDIT
DONE — the premise was WRONG: the ops are NOT unemitted. Instrumenting the whole
`-rt` run caught ~199 `EvalStmt` + 8 `EvalToSlot` + 2 `JumpIfFalse` across ~15
construct categories, so they CANNOT be deleted. The `-vm` differential is
SCRIPT-mode + inference-ON (the `repl:` tests are a separate list; `check()`
never sets `-nti`), so those fallbacks are NOT REPL/-nti — they are (1)
DELIBERATE error tests (the tree-walker produces the exact runtime error — an
undefined-NAME call `undefined_fn(..)`, a not-an-lvalue assign `true = false`),
plus (2) a SMALL set of genuine real-code construct gaps. Per-construct audit
(reproduced each in a normal correct script) — REMAINING SCRIPT-MODE REAL-CODE
GAPS: `foreach` over `array<bool>` (the flat bool byte needs a scalar read, no
`LoadElemBool`); a SLICE ASSIGNMENT `a[i:j] = [..]` (slice READS are native
`SliceV`, stores aren't); a NAMED nested func that CAPTURES an outer local (an
anonymous capturing lambda IS native, e.g. `adder`); a 3+-var dict `foreach`;
`D4`-overflow (>64 DISTINCT type signatures — the base tree-walks; rare, needs
65+ types). CONFIRMED NATIVE (not gaps): the whole exception path (P8), an
undefined-GLOBAL read (`LoadGlobalV` throws), compound dict-member assign,
multi-assign/swap, 2-var array-unpack `foreach`, discarded mutating builtins,
nested/boxed struct construction, closures + non-capturing nested funcs. The
BENCHES/SAMPLES compiled chunks are 100% native (verified `-vd` + a broad
structs/blocks/foreach/dyn/closures script → ZERO fallbacks). Tractable
categories nativized this step: **standalone `{ }` block statements**
(scope-free → `gen_stmts`; biggest category, -34) and **string `foreach`**
(`container_is_str` + `StrLen`/`LoadStrChar`). Op DELETION stays deferred (the
net is still needed for the error paths + `-nti -vm` + the few real gaps above);
"typical scripts fully native" is the achieved milestone. NEXT tractable
real-code gap: `array<bool>` foreach (a `LoadElemBool`)]**. Each step `-rt`
(differential) + samples byte-identical.

  **(e) continued — script-mode real-code sweep (2026-07-13).** Re-audited via
  the `ML_DBG_FB` `emit()` hook (compiled out by default; logs the rendered
  construct behind any fallback op) over the whole `-rt` run. Started at 161
  `EvalStmt` / 143 distinct; nativized, in order:
  - **`array<bool>` foreach → `LoadElemBool`** (binds a REAL bool, not 0/1;
    `ForeachStmt::elem_is_bool`). Single + indexed.
  - **Nested POD struct construction `L(P(1,2),P(3,4))` → `StructCtorV`** (gate
    widened to `pod_ctor_arg_safe`: a nested POD-struct-ctor arg of the exact
    field type; the nested ctor's value embeds via `pod_store_field`'s memcpy,
    coerce still can't throw). Recurses to any depth.
  - **Boxed (non-POD) construction `B(a,x)` → new `StructCtorBoxedV`**
    (`CallExpr::vm_struct_boxed_def`, copied onto the `DirectCallExpr` in
    `devirtualize_calls` — the missing copy is why it first didn't fire). A
    boxed field coerce CAN throw (dyn-launder), so **per-arg carets** are pooled
    in a serializable `Chunk::boxed_ctors` (`{def, ArgLoc[]}`) — byte-identical
    caret, AST-free. None-fills omitted trailing opt fields.
  - **Bare discarded-value expression statement** (`s[3];`, `a[i:j];`,
    `x + y;`) → `gen_stmt` compiles it to a scratch temp and drops the result
    (eval-for-value == eval-for-effect). SKIPS a bare leaf (Identifier / scalar
    Literal): the tree-walker never RValue-s a discarded statement, so an
    undefined name must stay its harmless `UndefinedId` no-op, not a
    `LoadGlobalV` throw.
  - **inc-dec used as a VALUE** (`y = x++`, `y = a[i]++`) → `compile_boxed_expr`
    `IncDecExpr` case: read-lvalue + mutate (postfix) / mutate + read (prefix),
    reusing the statement compilers; gated `incdec_lvalue_pure` (side-effect-
    free lvalue). This surfaced + fixed a **pre-existing auto-const bug**:
    `fold_reads` had no `IncDecExpr` case, so a promoted write-once INDEX var
    (`var i=1; a[i]++`) dangled — fixed by folding the inc-dec lvalue's READS.
  Result: **161 → 111 `EvalStmt`, 143 → 96 distinct.** `bench/` + `samples/`
  stay 100% native (empty `ast_nodes`). The residual 96 are: ~40 DELIBERATE
  error tests (undefined-name, rebind, not-callable, OOB, lvalue-builtin-on-
  literal, wrong-type/arity ctor — they throw, via the tree-walker for now),
  ~15 niche real shapes (member/dyn inc-dec statement, ≥3-level nested store,
  whole-`p` struct-array `foreach`, struct/nested-array literal in some
  contexts, IdList compound `+=`), 5 `InlinedCallExpr` block forms, `assert(..)`
  with an unliftable arg, and the dev-only `show` (script-excluded). Op DELETION
  still deferred (the error paths need native throwing ops first).

  **(e) error-path constructs → native throwing ops (2026-07-13).** The always-
  throwing constructs are now native via a new **`ThrowRuntimeV`** op + a
  serializable `Chunk::throws` pool (`{ThrowKind, Loc, name}`): it throws the
  pooled exception with the exact caret, byte-identical, AST-free. Nativized:
  - **undefined name** in an rvalue/callee position (`var y=foobar`,
    `undefined_fn()`, `undef(5)`, a `_` read) → `undefined_var` with the
    id/callee caret. A CALL with an unresolved callee throws BEFORE its args
    (matching `what->eval` first); a bare `foobar;` stays a no-op (the leaf
    guard).
  - **assign to a non-lvalue** — a scalar LITERAL (`0=99`, `true=false`, a
    const-inlined `K=6`) → `not_lvalue`, a BUILTIN (`print=5`) →
    `rebind_builtin`. The rhs compiles FIRST (side effects + its own throw),
    then the throw — matching the tree-walker's rhs-then-target order.
  - **lvalue-builtin on a non-lvalue arg0** (`append([1,2],3)`, insert/erase/
    pop/intptr) → `not_lvalue` at arg0's caret, after compiling the args.
    `builtin_requires_lvalue_arg0` EXCLUDES sort/rev_sort/reverse (they accept a
    value arg0 — a differential regression on `sort(clone(a))` caught this).
  3 loc-pinned err-loc tests (both engines). -rt EvalStmt 111→85.
  **(e) dyn-callee → a GENERIC value-call (2026-07-13, DONE).** An indirect call
  of a `dyn` callee (`var dyn a=len; a("hi")`, `a(1)` on a non-func) is native
  via **`CallValueGenericV`** (`CallExpr::vm_dyn_callee`). A first attempt (a
  `CheckCallableV` guard over `FuncObject` only) was REVERTED — a dyn callee may
  hold a `FuncObject`, a read-only `Builtin` (`len`), a MUTATING builtin (needs
  an lvalue arg), an AST builtin (`defined` — needs the unevaluated arg node),
  or a struct descriptor, so the dispatch is intrinsically AST-dependent and a
  fully AST-free lowering is IMPOSSIBLE. The op therefore KEEPS its CallExpr
  node (args + callee caret) but the callee LOAD is native and a `FuncObject`
  body runs native (the hook). The dispatch is the shared
  **`dispatch_call_value`**
  (eval.cpp), reused by the tree-walker's `CallExpr::do_eval` AND the op → the
  two engines are byte-identical over all six runtime callee kinds (func /
  read-only builtin / mutating builtin / AST builtin / struct / non-callable,
  incl. backtraces + arity/type errors). `extract_locs` records the op's
  CALL-SITE loc (for a FuncObject body's backtrace) WHILE keeping the node — the
  one op that does both. A Func-TYPED callee still uses the register-run
  `CallValueV`. -rt EvalStmt 85→83. The rest of the -rt error-ish inventory
  (`nonexistent=5`, `map(lambda,a)`, `P(1)` arity, `Point("s",2)`) reproduces
  NATIVE / compile-error standalone — context-only fallbacks, not constructs.

  **(e) niche STATEMENTS (2026-07-13, DONE the clean set).** Nativized: an
  **inc-dec STATEMENT on an int/float member/nested subscript** (`p.x++`,
  `a[i][j]++` == `lvalue += 1` → StoreMemberV / DictStore / StoreElem2V; gated
  on a proven int/float `th`, so a dyn field falls back — inc-dec is int/float-
  only, `d++` on a string throws but `+= 1` would concat); its **VALUE form**
  (`o = p.x++`, `incdec_lvalue_pure` widened to member/nested); a **whole-`p`
  flat-struct-array `foreach`** (**`LoadStructElemV`** materializes a fresh
  StructObject per iteration — byte-identical to the tree-walker's reused-object
  bind, since its COW guard only avoids overwriting a captured/stored element;
  scalar-field bodies keep the direct read); a **compound multi-target assign**
  (`a, b += rhs` — `MultiUnpackV` gained a base op: read each target, apply,
  write back, `_` skipped). **STILL fallback** (harder/rarer): a member-subscript-
  member nested store (`d.a[0].f=v` — a genuine `NotLValueEx` path in both
  engines), a dyn **MEMBER** inc-dec statement (`d.f++` — almost always a
  `NotLValueEx` on a POD field). NONE in bench/ or samples/ (all native).
  **(f) the remaining niche (2026-07-13, DONE).** `append`/`push` to a struct
  MEMBER (`CallBuiltinLVMember`), a ≥3-level / generic nested store
  (`StoreElemChainV`), the `_`-in-unpack `foreach` (`UnpackElemTargets` — a
  per-position `Chunk::unpack_targets` slot map, `-1` == `_`), the INDEXED dict
  `foreach` (`foreach (i, k[, v] in indexed d)` — the live-iterator lowering
  plus an int index counter; also fixed a latent front-end mis-typing of the
  value as the key type), the dyn SCALAR inc-dec (`IncDecCheckedV`, int/float-
  checked), and the dyn ELEMENT inc-dec (`c[k]++` → `IncDecElemCheckedV`: forms
  the element LValue via the runtime subscript, int/float-checked; KEEPS its
  node for its TWO error carets — the subscript loc for a subscript-internal
  KeyNotFound/OOB vs the inc-dec loc for its own NotLValue/TypeError, which the
  one-loc side table can't both hold).
  **(g) the `InlinedCallExpr` block form (2026-07-13, DONE the common case).**
  A `y = f(args)` whose block-bodied `f` inlined with a residual that couldn't
  collapse to a ternary (a leading side-effecting statement / a reassigned
  local) stays an `InlinedCall(Block(...))` - the body run behind its OWN return
  boundary. Lowered via a **scoped return boundary**: an `inline_returns`
  codegen stack (like the loop stack); `compile_boxed_expr` inits a result slot
  to none, compiles the body inline, and `try_native_return` (gated on
  `inline_returns`) redirects each `return v` to "MoveV into the result slot +
  Jump to the body end" instead of ReturnV-ing the whole chunk. A return that
  crosses a try INSIDE the boundary fails the whole inline -> the tree-walker
  runs it (byte-identical). FIXED a latent bug this surfaced: `make_typed`
  (specialize) hand-copied base fields and DROPPED `inline_ctx`, so a
  specialized arith node inside an inlined body lost its inlined-at chain (the
  tree-walker hid it - the Block wrapper flushes - but the VM's flat body has no
  wrapper); now uses `copy_base_fields`, so an error inside a native InlinedCall
  shows the byte-identical virtual `f$0` frame. STILL fallback: an
  InlinedCallExpr whose return crosses a try inside the boundary (rare).
  **(h) typed NON-SCALAR decls/assigns (2026-07-13, DONE).** An explicitly-typed
  `str`/`bool`/`array<…>`/`dict<…>`/struct decl/reassign/compound (`str s = ..`,
  `array<int> a = ..`, `Point p = P(..)`, `s += ".."`, global/capture/const
  forms) fell to `EvalStmt` because `compile_boxed_stmt` declined ANY
  `decl_type ∉ {none,dyn}`. But `coerce_to_decl_type` is a NO-OP for every type
  EXCEPT int/float (it returns the value unchanged; the type is proven at compile
  time), and the tree-walker's decl path only coerces for `DeclType::i`/`f`. So
  the gate now declines ONLY `i`/`f` (numeric widen / dyn-narrow throw, whose
  native scalar case is compile_int/float_stmt); everything else is a plain boxed
  store, byte-identical. Flat array/dict storage is preserved (the ArrHint rides
  the rvalue), and an uninitialized typed decl is already a zero-value/zero-ctor
  rvalue at parse. STILL fallback: an int/float-typed decl fed a `dyn` value
  (left to the coerce).

### CANONICAL residual fallback list (2026-07-13, after step (h))

The script-mode constructs that STILL keep an `ast_nodes` entry (an
`EvalStmt`/`EvalToSlot`/`JumpIfFalse`), assessed from the codegen dispatch, most
-likely-to-appear first. These are what remains between "bench/samples are 100%
native" and "ALL scripts serialize with an empty `ast_nodes`".

1. **AST builtins in value/statement position** — ✅ **DONE (2026-07-13)** for
   the live residual. `isconst`/`isconstdecl` always FOLD (`fold_isconst`);
   `ispure`/`ispuredecl` are native (`make_builtin_v`, `func_v`);
   `type`/`decltype`/`typestr`/`kindstr` fold/elide or are native (dual-ABI
   `func_v`); `show` is compile-rejected in scripts (dev-only). `defined` was the
   real holdout: `defined(local/param/capture/builtin)` folds to `true`,
   `defined(global)` → `DefinedGlobalV`, and now `defined(<never-declared>)`
   folds to `false` in a SCRIPT (`try_fold_defined` — the runtime map is empty +
   asserted, so an unresolved name is never defined; byte-identical to
   `arg->eval` → UndefinedId; NOT in the REPL, where it may be a live map
   global — `AutoConst` gained a `repl_mode` flag). RESIDUAL: only a REPL
   unresolved `defined`, and the rare `defined(<non-identifier>)` misuse
   (`defined(a[0])`).
2. **`const` reassignment of a runtime const** — ✅ **DONE (2026-07-13)**. A
   plain OR compound rebind of a `const`-bound func/array/dict (`const c = [..];
   c = x` / `c += x`) now lowers to a native `ThrowRuntimeV` with a new
   `ThrowKind::rebind_const` → `CannotRebindConstEx`. `compile_boxed_stmt`
   compiles the rhs FIRST (its side effects run) then emits the throw with the
   lvalue's caret — byte-identical to the tree-walker's rhs-then-throw order (a
   const *scalar* is inlined, so its rebind is the bad-lvalue throw instead).
3. **Two residual inc-dec / store shapes** — a dyn **MEMBER** inc-dec (`d.f++`):
   ✅ **DONE (2026-07-13)** via `IncDecMemberCheckedV` (the twin of
   `IncDecElemCheckedV` — forms the member LValue like `MemberExpr::do_eval`'s
   rooted-base path, int/float-checked, dual-loc node-kept; a dict value / boxed
   struct field works, a POD field / missing key throws exactly as the tree-
   walker; also covers a proven-struct non-numeric member). The **member-in-the-
   middle nested store** `a[i].f=v` / `q.p.x=v` / `s.f[i]=v` / `d.a[0].f=v`: ✅
   **DONE (2026-07-13)** via `StoreLValueChainV`. `try_native_chain_store`
   decomposes the lvalue into a slotted base + a `Chunk::chain_steps` list (a
   member = a member_keys pool idx, a subscript = a pre-evaluated key temp, each
   with its own node loc). `vm_chain_lvalue_store_op` carries `cur` as an
   `LValue*` ref OR a plain VALUE — the tree-walker's chained do_eval, where an
   immutable intermediate is a value READ the walk continues on, failing
   NotLValue only at the FINAL store, so `q.p.x` on nested-POD carets the whole
   lvalue). The final step dispatches struct (`vm_member_store`) / dict member
   (`vm_subscript_store(memId)` == `d["f"]=v`, auto-vivify) / subscript
   (`vm_subscript_store`); per-step locs make ALL carets byte-identical. Works
   for boxed structs / dict values, throws NotLValue for POD, byte-identical.
   Only an optional `d?.f++` still falls back (rare). **The pre-existing
   StoreElem2V/StoreElemChainV single-loc imprecision (an inner-subscript throw
   showed the whole `a[i][j]` span) is now FIXED too** (2026-07-13): both carry
   per-step subscript carets in a new `Chunk::chain_locs` pool (a `{Loc,Loc}`
   per step, deref only on the throw path — a pointer, no hot-store regression:
   matrix -vm 0.18s unchanged).
4. **`foreach` where all six handlers decline** — a non-local loop/unpack var
   (global/capture), an **indexed** dyn-container foreach, a **>2-var** dyn
   foreach, a container not proven array/str/dict/dyn (an `opt` container, or a
   struct-typed one outside the flat-struct fast path), or a body
   `compile_scalar_body` can't lower.
5. **`for` / `for-range` declines** — a **float loop var** (for-range doesn't
   specialize it yet), or an init/cond/inc/body that can't lower.
6. **`InlinedCallExpr` whose `return` crosses a `try` INSIDE the boundary** —
   rare; the scoped-return-boundary lowering bails.
7. **Non-scope-free `Block`** — a block declaring a capturing closure / nested
   named func, or overflowing the slot budget: it needs its own `EvalContext`
   (which `vm_run_chunk` doesn't build), so only `scope_free` blocks inline
   (`gen_stmt:5327`).
8. **Capturing named func decl** — bound to a *local* slot via `declare_masking`
   (not a global slot), so the `MakeClosureV`+`StoreGlobalV` path (global-only,
   `gen_stmt:5269`) declines. Plus the edge declines of `throw`/`try`/`return`
   on an un-liftable value/flow.

**NOT a distinct category (a clarification of an earlier note):**
- **"Nested-in-native"** — `compile_scalar_body` deliberately lets a *flow-free*
  statement it can't lower run as an `EvalStmt` INSIDE an otherwise-native loop
  (rather than failing the whole loop), keeping the loop native around it. That
  is a good *mechanism*, but the fallback statement itself is STILL one of
  1–8 above and STILL keeps an `ast_nodes` entry — so for the empty-`ast_nodes`
  goal it is NOT free; it is exactly residual 1–8 relocated into a loop body.
  (The earlier "not a real script gap" phrasing was about the loop-nativization
  mechanism, not the serialization count — corrected here.)
- **REPL-only** — every top-level `FuncDecl`/`StructDecl`/global assign is
  map-resident in the REPL, so all fall back; a *script* slots them natively.
  Irrelevant to `.myv`.
- **Whole-function tree-walk** (coarser than an `EvalStmt`): a function whose
  body is non-scope-free (captures / nests a func — i.e. residual 7/8 at the
  body level) gets NO chunk at all (`codegen_func_body`'s gate) and runs entirely
  tree-walked. That function has zero bytecode, a *bigger* serialization gap than
  a single `EvalStmt`, and is tied to residuals 7/8.

- **Dropping the `ast_nodes` POOL for a pc-keyed side table** — ✅ **DONE
  (2026-07-13).** `Instr::node_idx` stays the splice-STABLE handle codegen needs
  (ops grow + roll back before an op's final pc is known, and an index survives
  that where a pc would not), indexing a now CODEGEN-TRANSIENT `ast_nodes`.
  After `extract_locs` (post-assembly, pcs final), **`build_node_table`**
  flattens surviving `{pc → ast_nodes[node_idx]}` into a pc-keyed `node_table`
  (`{pc, Construct*}`, like `locs`/`inline_ctxs`, binary-searched by
  `node_at_pc`), NULLS every live `node_idx`, and CLEARS `ast_nodes`. So the
  finished chunk carries NO indexed pool and NO live per-`Instr` `node_idx`; the
  runtime looks a residual node up by pc on the cold path only. ONE `Instr`
  layout (no codegen-vs-runtime split — `node_idx` is a codegen-only handle,
  always `-1` at runtime). All `bench/` + `samples/` keep an EMPTY node_table
  (the struct benches' last `EmplaceStruct` node moved into the serializable
  `emplace_sites` pool — see the execution-order step 2 below).
  `node_table` is now the LAST non-serializable side table (the audit
  signal for a `.myv` writer).

## ⛔ THE COMPLETE AST-RETENTION INVENTORY (2026-07-13, code-derived)

Supersedes the canonical list above for the ZERO-AST goal. Compiled by reading
every codegen dispatch path (no `-vd`, no `ML_DBG_FB`). A `-vm` script retains
AST in FOUR tiers, not just the fallback-op list — all four must reach zero for
`.myv`. Codegen input is always SCRIPT-mode trees (the REPL never calls
codegen; `-rt`'s differential runs `check()` in script mode), so every
reachable path below is script-reachable unless proven otherwise.

### Tier 1 — fallback-op EMIT SITES in codegen.cpp (re-enter the tree-walker)

(Line numbers are as of 2026-07-13; residues struck as they land.)

1. **`emit_init`:726 → EvalStmt** — loop init failing int+float+boxed stmt
   compile. ~~a typed i/f decl fed dyn~~ (CoerceNumV, step 6); residue: an
   rhs `compile_boxed_expr` declines (shrinks with every expr lowering).
2. **`eval_to_temp`:2086 → EvalToSlot**, from `compile_int_expr`:2952 /
   `compile_float_expr`:3719 — a `th==i/f` `DirectBuiltinCallExpr` failing
   `try_native_builtin`: ~~`defined(a[0])`~~ (native, step 3); residue:
   `emit_args_range` failing on an arg (recursive; shrinks).
3. **`compile_scalar_body`:4240 → EvalStmt** — a flow-free Expr14/CallExpr/
   Return/Throw inside a native region that every native path declined (each
   such shape is itself one of the residues here; the mechanism keeps the
   loop native around it).
4. **`compile_native_if`:4298 → JumpIfFalse** — an `if` cond failing
   int+float+boxed compile (shrinks with every expr lowering).
5. **`gen_stmt`:5404 → EvalStmt** — ForRangeStmt declined: ~~a non-operand
   step~~ (compiled-temp, step 5); residue: a bound/step that even the boxed
   path can't compile; body failure. (The init-order WRONG-RESULT bug found
   here is FIXED — see step 5.)
6. **`gen_stmt`:5409 → EvalStmt** — ForStmt declined: ~~NO cond~~,
   ~~a boxed-only inc~~ (both native, step 5); residue: cond failing
   `emit_cond_jumps`; body failure.
7. **`gen_stmt`:5419 → EvalStmt** — Foreach: all six handlers decline
   (indexed dyn container, >2-var dyn, container not proven
   array/str/dict/dyn e.g. `opt`, non-local vars, body failure).
8. **`gen_stmt`:5542 → EvalStmt** — the catch-all: ~~typed i/f decl fed
   dyn~~ (step 6); ~~bare `defined(a[0]);`~~ (step 3); residue: `d?.f++`;
   InlinedCallExpr w/ a return crossing an inner try; a NON-scope-free
   standalone Block; a FuncDeclStmt bound to a LOCAL slot (ONLY reachable
   via the brace-less-body CRASH — see the rescoped step 4: once the parser
   normalization lands this residue is provably dead and the path is
   deletable); IdList destructure w/ a typed/const target; Return/Throw/Try
   declines.
9. **`gen_if`:5548/5551/5556 → JumpIfFalse + EvalStmt×2** — reached only
   when `compile_native_if` failed (= a BRANCH failed `compile_scalar_body`);
   then the whole cond + both branch Blocks fall back.
10. **`gen_while`:5570/5573 → JumpIfFalse + EvalStmt** — reached when
    `try_native_scalar_while` failed (cond or body); the whole cond + body
    fall back.

Notes: `compile_native_try` itself declines ONLY on (a) a non-slot catch var
(REPL-only — script catch vars are always frame slots) or (b) a body/catch/
finally `compile_scalar_body` failure (recursive residue). `break`/`continue`
crossing NESTED trys (`emit_break_cont` false) and a `return` crossing >1 try /
any finally fail the enclosing body (→ sites 5/6/10). Sites 9/10 are STRUCTURAL
waste even before full elimination: they fall back the WHOLE branch/body where
per-statement dispatch (the `gen_stmt` catch-all granularity) would keep the
compilable statements native.

### Tier 2 — native ops KEEPING a `Construct*` at runtime (node_table)

Exactly the `extract_locs` KEEP list + `build_node_table` survivors (verified
against every `node_at_pc` consumer in vm.cpp):

1. `EvalStmt` / `EvalToSlot` / `JumpIfFalse` — the Tier-1 ops themselves.
2. ~~**`EmplaceStruct`**~~ — ✅ AST-free (step 2: the `emplace_sites` pool —
   was the ONLY node in all of `bench/` + `samples/`; the corpus now dumps
   ZERO node_table sections).
3. ~~**`IncDecElemCheckedV`**~~ — ✅ AST-free (step 1: `incdec_sites`).
4. ~~**`IncDecMemberCheckedV`**~~ — ✅ AST-free (step 1).
5. **`CallValueGenericV`** — vm.cpp:2205. The CallExpr node feeds
   `dispatch_call_value`: a DYN callee may resolve to a Builtin whose ABI takes
   the UNEVALUATED `ExprList` (`defined`/`isconst`/`decltype` need lazy args;
   an lvalue builtin needs arg0 as an LVALUE). NOT poolable as-is — the
   maintainer DECIDED the fix (2026-07-14): see fork F1 in the fork list.
   **THE LAST non-fallback node-keeping op.**

### Tier 3 — whole bodies with NO chunk (100% tree-walked, coarser than any op)

From `codegen_func_body`'s gates (codegen.cpp:5859):
1. **Non-scope-free function bodies** (`!body->scope_free`: a capturing
   closure / nested named func / slot overflow in the body) — the WHOLE
   function runs `do_eval`. The body-level twin of the non-scope-free Block.
   (D2 proved every SCRIPT block scope-free post-F2, so in a script this is
   empty; the gate stays as the safety net.)
2. **Expression-bodied functions** — ✅ DONE (2026-07-14): `func f(x) =>
   expr` is now parse-time SUGAR for `{ return expr; }` (maintainer
   decision) — the parser's arrow branch desugars, so EVERY function body
   is a Block and compiles to a chunk (a `ReturnV` over the native expr;
   `codegen_func_body`'s is_block gate passes naturally). The passes that
   optimized the sugar look through the wrapper via **`func_expr_body`**
   (syntax.h — matches the hand-written twin too, deliberately: the two
   spellings are indistinguishable): the EXPRESSION inliner (classification
   + splice + `-it` size gate all on the inner expr — byte-identical
   optimizer decisions, `-a` coloring diff-verified empty), `do_func_call`'s
   direct-eval fast path (tree-walker perf preserved; under `-vm` the chunk
   runs — the native form), coderender (`=> expr;` rendering). Two Inliner
   rules found + fixed during this: a `funcs`-registered (expr-engine) func
   stays OUT of `block_funcs` (block-inline is CALL_WEIGHT-gated and
   bypassed `-it`), and **`refold` never folds a LAZY builtin**
   (`defined(gg)` tolerates UndefinedId, so a cctx eval "succeeded" with a
   compile-time answer to a runtime-order question — caught by the
   defined-order test the moment hand-written `{ return defined(gg); }`
   became expr-inlinable).
3. The **all-fallback gate** (no REAL op → no chunk) — self-erasing once
   Tier 1 empties, but until then a fallback-heavy body is chunk-less.
4. Base templates (`is_template_base`) — correct (dead code, never runs).

### Tier 4 — the runtime call model itself is AST-anchored

1. **`Chunk::closure_defs` holds `const FuncDeclStmt *`** — a `Construct*`
   POOL, explicitly against the ZERO-AST rule ("pooled data is plain values …
   NEVER a Construct*"). `MakeClosureV` builds `FuncObject(def, &ctx)` from it.
2. **`FuncObject` / `do_func_call` read the `FuncDeclStmt` at call time**
   (params via the param `Identifier` nodes, body, frame_size, cache flags) —
   EVERY function call is AST-anchored, chunk or not.
3. **`StructTypeDef*`** (struct_defs / consts / literal_objs.arr_hint_struct /
   boxed_ctors) — NOT a `Construct`, but AST-OWNED (by its `StructDeclStmt`),
   so freeing the AST today would dangle it. `.myv` needs the defs owned by
   the program image, not the tree.
4. **`Chunk::node_table`** — the `{pc, Construct*}` side table itself; must
   END EMPTY for every script chunk and then be deleted outright.

Zero-AST therefore = empty Tier 1 (every construct lowers) + pooled Tier 2 +
chunked Tier 3 (VM scope ops for non-scope-free bodies) + a serializable
FUNCTION DESCRIPTOR replacing `FuncDeclStmt`/`closure_defs` at runtime
(Tier 4) — at which point the AST is freed after codegen and `node_table`
is deleted.

### Execution order (each its own commit)

1. ✅ **DONE (2026-07-13)** Pool the IncDec dual carets → ops 3/4 AST-free:
   the serializable `Chunk::incdec_sites` pool ({lvalue caret, inc-dec
   caret, memId, memUid}, `Instr::b` = the index, O(1) — faster than the
   old `node_at_pc` binary search); the undefined-global-base caret via the
   loc side table (`vm_store_base`, node=null). Dual carets + the undefined
   base now PINNED by four `err loc:` tests; `ast_node_pool_minimal` case
   (c) asserts an empty node_table for both shapes.
2. ✅ **DONE (2026-07-13)** Pool EmplaceStruct (def + carets) → op 2
   AST-free via the serializable `Chunk::emplace_sites` pool ({ctor POD
   def, callee name, container-arg caret, per-field coerce carets},
   `Instr::a` packs `kind | idx << 2`); the whole-args caret rides the loc
   side table (`vm_stamp_loc`). vm_emplace_struct's signature is now
   node-free. VERIFIED: a `-vd` sweep over ALL of `bench/my/*` +
   `samples/*` shows ZERO `node_table` sections — the whole corpus is
   100% AST-free; `ast_node_pool_minimal` case (d) pins it (and its first
   draft dereferenced the AST-owned StructTypeDef after freeing the tree —
   ASan caught it, a live proof of Tier-4 item 3's lifetime rule).
3. ✅ **DONE (2026-07-14)** `defined(<non-identifier>)` = eval-arg-then-true
   (builtin_defined only tests UndefinedId, whose SOLE producer is
   Identifier::do_eval — proven by grep) → `try_native_defined_expr`
   (compile the arg + LoadConstV true, wired into compile_boxed_expr AND
   compile_int_expr's builtin chain). A WRONG-ARITY `defined(a,b)` (throws
   BEFORE evaluating args) → `ThrowRuntimeV` kind `bad_args` →
   InvalidNumberOfArgsEx, args caret. Tests: value+effects, discarded
   statement, arg-throw caret pin, wrong-arity caret pin, node_table-empty
   pool case (e). Every `defined` form is now fold/native.
4. ~~FuncDeclStmt → LOCAL slot: MakeClosureV + slot bind~~ — **RESCOPED
   (2026-07-14): it is a pre-existing LANGUAGE BUG, not a lowering gap.**
   A named func can NEVER capture (the grammar rejects `func f[x]`
   everywhere), so the only script route to a masked (local-sym) named
   func is a func decl as a BRACE-LESS `if`/loop body —
   `hoist_scoped_decls` only scans Block statement lists, so the name is
   `declare_masking`'d and `FuncDeclStmt::do_eval`'s `ctx->emplace` hits
   the asserted-empty script map: `if (c) func g() => 1;` and
   `while (i < 1) func g() => 1;` ABORT the tree-walker today (braced
   forms work — they hoist to scoped globals). **DECIDED (2026-07-14),
   option (b): the parser normalizes brace-less bodies to implicit
   Blocks** — see fork F2 below for the full fix sketch. Once fixed,
   `gen_stmt`'s non-global-FuncDeclStmt fallback is provably DEAD in
   scripts (the REPL never runs codegen) → delete it.
5. ✅ **DONE (2026-07-14)** for(;;) + boxed inc + non-operand for-range
   step — all native. AND a real WRONG-RESULT `-vm` bug found + fixed
   while here: `try_native_for_range` compiled a non-trivial bound temp
   BEFORE the init, but `ForRangeStmt::do_eval` runs init → bound → step,
   so a side-effecting init that changes the bound diverged
   (`for (var i = drop(x); i < len(x); i++)` gave 3 on -vm vs the
   tree-walker's 2). The init now emits FIRST; the step gets the same
   compiled-temp path as the bound (a subscript-read step `i += st[0]`
   lowers); `for (;;)` is an unconditional native loop (exit via
   break/return); a boxed inc (`out += "x"`) uses the three-tier
   statement dispatch. Four new tests incl. the order pin.
6. ✅ **DONE (2026-07-14)** Typed i/f coerce store → **`CoerceNumV`**
   (dst = coerce_to_decl_type(src) — the exported vm_coerce_decl_num, so
   widen/none/narrow-throw is byte-identical by construction; a LOCAL
   lvalue fuses coerce+store, a global/capture coerces into a temp; caret
   = the Expr14 span via the loc table). The live producer is the
   coerces_dyn accumulator (`var s = 0; s = s + d`) — the last EvalStmt
   in a plain accumulator body; an EXPLICIT `int x = <dyn>` is
   compile-rejected (TypeMismatchEx) and a COMPOUND doesn't coerce
   (measured: `s += runtime(2.5)` stores 2.5 in BOTH engines - the
   documented op==assign-only coerce), so those lower as-is. Pool-test
   case (f) + a caret pin + float/global/bool-widen tests.
7. ✅ **DONE (2026-07-14)** Nested-try flow + inline-return-across-try.
   AUDIT CORRECTION: flow across NESTED trys was ALREADY general —
   `inline_crossed_finallys` chains every crossed try's handler-pop +
   finally innermost-first at any depth (the "at most ONE try" comment
   was STALE; deleted, behavior pinned by a 2-finally break/return
   test). The real residue was the `InlinedCallExpr` return crossing a
   try INSIDE the boundary (reachable: a ≤CALL_WEIGHT body like
   `func g(x) { try { return x+1; } finally { t++; } }` block-inlines;
   its return bailed the whole inline to EvalStmt). Now
   `try_native_return`'s boundary branch inlines the crossed finallys
   (bounded at try_base, value copied to a protected temp first — the
   finally may overwrite it, pinned) then MoveV+Jump. The
   compile_scalar_body decline path stays as the safety for an
   uncompilable value/finally.
8. ✅ **DONE (2026-07-14)** Foreach residual shapes. The ForeachDyn
   iterator is now GENERAL over the id list (any var count, `indexed`,
   `_` placeholders): the per-var slots ride an unpack_targets pool
   entry, Init packs `nvars | indexed << 8`, Next binds from the state
   exactly as do_iter (array single-bind / strict N-unpack; dict
   key/value/none-pad; indexed counter in targets[0]). The inferencer
   stamp widened to any-var/indexed dyn containers. "Unproven container"
   has NO valid-script residue (an `opt` container foreach is
   compile-rejected — NullabilityEx; verified). A non-local loop var
   (global/capture) still falls back (rare). AND another pre-existing
   CRASH found+fixed: a 1-var `indexed` foreach over an ARRAY
   (`foreach (i in indexed a)`) read `ids->elems[1]` OUT OF BOUNDS in
   do_iter's single-bind (abort under container hardening, UB in plain
   release; the dict path handled the same shape fine) — now binds
   nothing beyond the index, both engines, pinned.
9. Restructure gen_if/gen_while onto per-statement granularity, then DELETE
   each Tier-1 site as its residue provably empties.
10. THE DESIGN FORKS — **DECIDED by the maintainer (2026-07-14)**; each
    below with its agreed fix, ready to implement.

### FORK DECISIONS + fix sketches (maintainer-approved, 2026-07-14)

**F1. `CallValueGenericV` (the last non-fallback node op) — DECIDED: a
LAZY-ARG builtin cannot be called INDIRECTLY (a language rule).
STEP 1 ✅ DONE (2026-07-14): the rule.**
- The problem: a dyn callee resolving to a LAZY builtin needs the
  UNEVALUATED args (`var dyn f = defined; f(x)` must not evaluate `x`),
  which pre-evaluated register-run values cannot reproduce. The lazy set
  was pinned by reading each runtime body: **`defined` / `isconst` /
  `isconstdecl`** (pure NODE properties). `decltype` is NOT lazy after
  all — like `type`/`typestr`/`kindstr` it is dual-ABI (its `func_v`
  builds from the runtime value), so all four type queries stay usable
  as values; `show` was already script-rejected.
- IMPLEMENTED as a COMPILE-TIME reject only (`is_lazy_builtin` +
  `mark_lazy_builtin` in types.cpp; the value-use check rides the
  `reject_dev_builtins` walk, same `g_dev_builtins_allowed` gate): in a
  script, a lazy builtin's name in any position but a direct-call callee
  is a `SyntaxErrorEx` ("takes an unevaluated argument; it cannot be
  used as a value"). The originally-sketched RUNTIME check in
  `dispatch_call_value` was dropped as unnecessary-and-harmful: a script
  can no longer PRODUCE such a value (every route into a dyn is a value
  position), and the REPL — which retains the AST, so the indirect form
  genuinely works there — would have been broken gratuitously (the
  `show` precedent). README documents the rule under `defined` +
  `isconst`.
- **STEP 2 — DECIDED (2026-07-14). The blocker analysis was CORRECTED by
  measurement before deciding; the earlier COW claim was WRONG:**
  * VERIFIED FACTS (both engines, pinned by probes): (i) plain assignment
    ALIASES (`var b = a; b[0] = 9` bleeds; same intptr); (ii) a regular
    function CAN mutate an array param — the param is a VALUE-copied
    handle and `v[0]=99` / `append(v,55)` reach the caller's array — so a
    value-passed arg0 to append DOES mutate the original for a PLAIN
    array (the earlier "use_count>1 → COW clones → silent no-op" claim
    was false; the ban recommendation is WITHDRAWN); (iii) the ONE real
    lvalue dependence is the SLICE WRITE-BACK: direct `append(slice, 9)`
    clones + writes the new array back INTO THE SLOT (`s` becomes
    [1,2,9]) while the same mutation through a value copy leaves the
    caller's slice untouched; (iv) const is enforced by the DEEP
    read-only flag, which travels with the value (not lvalue-dependent).
  * DECISION (maintainer): (1) indirect `map`/`filter` get EAGER-ARGS
    semantics (the direct form keeps the tested validate-before-arg1
    order); (2) NO ban and NO new syntax (`&`/byref) — mylang's by-ref
    encoding already EXISTS as a VALUE: an `EvalValue` holding `LValue*`
    (the internal `t_lval`), which is exactly what `Identifier::do_eval`
    hands a builtin today. The VM learns to PRODUCE that value natively:
    indirect-call arg0 compiles in LVALUE-PRESERVING mode (a slotted id →
    box `&slot`; a subscript/member → the element/field `LValue*` via the
    runtime `subscript(for_write=false)` / `vm_member_lvalue`, the
    CallBuiltinLVElem formations; anything else → a plain value). The
    shared dispatch hands a `func_lv` callee `args[0]`'s `LValue*` (null →
    NotLValueEx) — so slice write-back, const, and literal-arg0 errors
    reproduce byte-identically, because the builtin receives the same
    `LValue*` it gets today.
  * ✅ **STEP 2 LANDED (2026-07-14).** As decided, with two
    implementation-time discoveries that refined the design:
    - Frame slots can't hold an LValue*-boxed value (LValue::type_checks
      also bans t_undefid) — the first cut (three lvalue-preserving load
      ops materializing raw values into the run) ASSERTED immediately. The
      final design carries arg0 as a **DESCRIPTOR** in the new
      `Chunk::CallSite` pool (forms none/slot/elem/member/undef + the
      ArgLocs carets): the dispatch RE-DERIVES the LValue* only for a
      func_lv callee (the CallBuiltinLV model); an elem/member arg0's
      VALUE fills run[0] via the ordinary SubscriptV/MemberV at its
      position (throws keep argument order; the elem INDEX rides a
      reserved temp; the func_lv re-derive repeats the subscript —
      idempotent, and a between-args container mutation THROWS where the
      tree-walker's stale LValue* is UB, safer). Slice write-back / const
      / literal-arg0 of an indirect append/sort are byte-identical
      (pinned).
    - `dispatch_call_value` serves EVERY tree-walked builtin call
      (const-eval, REPL, unspecialized direct calls), not just indirect
      ones — the first eager cut hijacked direct `sort(a)` in const-eval
      (UBSan caught a null-LValue deref). The eager path is gated on the
      inferencer's `CallExpr::vm_dyn_callee` stamp — the true "indirect"
      marker; direct tree-walked calls keep the node ABI (map's
      validate-first, sort's custom arg0).
    Mechanics as decided: `Builtin::Kind` (value/lvalue/map/filter/lazy/
    node; 16→24 bytes, inside the EvalValue payload), `CheckCallableV`
    before the arg run, `construct_struct_v`, and ONE shared
    `dispatch_builtin_values` both engines call. Tests: slice write-back,
    elem-arg0 re-derive, literal/const arg0 errors, the eager-vs-direct
    map order pin, indirect struct arity, pool case (g). KNOWN CORNER
    (documented): an UNRESOLVED id in a non-arg0 position throws at its
    position under the VM but at the consumer in the tree-walker (later
    args' side effects may differ; same exception either way). **Tier 2
    is now ONLY the three fallback ops** — every native op is AST-free.

**F2. The brace-less-body func/struct decl CRASH — ✅ DONE (2026-07-14,
`pWrapDeclBody` in parser.cpp).**
- The bug (reproduced): `if (c) func g() => 1;` and
  `while (i < 1) func g() => 1;` ABORTED the tree-walker —
  `hoist_scoped_decls` only pre-scans Block statement lists, so the decl
  was `declare_masking`'d and `FuncDeclStmt::do_eval`'s `ctx->emplace`
  tripped the asserted-empty script map (`in_const_eval() || repl_mode`,
  eval.cpp:147). Braced bodies hoist to scoped globals and work. (A named
  func can NEVER capture — the grammar rejects `func f[x]` — so this
  crash was the ONLY script route to a masked named func.)
- **The implemented fix is NARROWER than first proposed**, because
  implementation-time probing disproved the "semantics-neutral" premise
  of a blanket wrap: a brace-less body DECL **leaks into the enclosing
  scope by long-standing behavior** (`if (c) var x = 5; print(x)` → 5;
  `if (c) const K = 7;` keeps K; a `while`-body `var` too). So
  `pWrapDeclBody` wraps a brace-less body in a synthetic single-statement
  Block ONLY when it is a FuncDeclStmt/StructDeclStmt (the crashing
  shapes, which nothing could have depended on); var/const bodies keep
  the leak (pinned by tests). Two more preserved subtleties: the `if`
  wraps only the RUNTIME statement — a const-true
  `if (true) func g() => 1;` still folds to the bare decl (the
  feature-flag pattern keeps g visible); and a PURE expr-bodied func is
  const-folded at its call sites REGARDLESS of scope (a pre-existing
  fold-vs-scope quirk, both engines identical — block-scoping tests must
  use an IMPURE func).
- With the masked route gone, a SCRIPT named func/struct decl ALWAYS has
  a global slot: `gen_stmt` now `ML_CHECK`s that invariant (the EvalStmt
  fallback for it is deleted). Tests: the crash shapes (if/for/while +
  struct), braced-equivalence (impure g → UndefinedVariableEx after the
  block in BOTH forms), and the leak pins. Samples unaffected (swept).

**F3. Tier 3/4 — the `.myv` load-bearing architecture — DECIDED: design
first, in its own plan file, before any code.**
- Scope: VM scope ops (a child-EvalContext push/pop or a resolver change)
  for non-scope-free blocks AND bodies; a SERIALIZABLE function descriptor
  replacing `FuncDeclStmt` at runtime (params/frame_size/flags as pure
  data + a chunk reference) so `closure_defs` stops holding `Construct*`
  and `do_func_call` stops reading param `Identifier` nodes; expression
  bodies compiled (a one-`ReturnV` chunk); `StructTypeDef` ownership moved
  from the `StructDeclStmt` to the program image (the test-caught UAF in
  `ast_node_pool_minimal`'s first draft is the demonstration: deref of an
  AST-owned def after the tree died).

### TIER-1 ENDGAME AUDIT (2026-07-14, post F1/F2 — the remaining
### reachable fallback shapes, probed + code-verified)

The three fallback ops' remaining REACHABLE feeders, after everything
above landed. Probes pinned each claim (both engines + `-vd` on crafted
scripts).

**Live expression ROOTS (every recursive residue — loop init/cond/inc/
body, return/throw values, try bodies, call args — funnels into these):**
- R1 **`CoalesceExpr`** (`a ?? b`) — ✅ DONE (2026-07-14): a
  `compile_boxed_expr` case lowers it to MoveV(lhs→dst) +
  **`JumpIfNotNoneV`** (a new op: skip the rhs when dst is non-none) +
  the rhs into the same dst — short-circuit preserved (pinned by the
  se()-counter test). The dst is reserved BELOW the scratch temps so
  the rhs compile can't clobber it.
- R2 **Chained comparisons** (`a < b < c`, `a == b != c`) — ✅ DONE
  (2026-07-14): the `emit_boxed_chain` k=='c' 2-operand limit was
  simply removed — the chain loop already accumulates left-to-right
  (CmpV per step, bool promoting), exactly the tree-walker's order.
- R3 **Assignment as an EXPRESSION / chained assign** (`x = y = 5`) —
  ✅ DONE (2026-07-14): a compile_boxed_expr `Expr14` case for a
  resolved-LOCAL non-const id target dispatches the int/float/boxed
  STATEMENT compilers, then yields the target's slot as the operand.
  This exposed + fixed a RETARGET-GUARD bug: the plain-assign retarget
  (`var a = <rvalue-op>` steals the op's dst) must require the op's
  dst to be a TEMP (`rslot >= temp_base`) — without that,
  `a = (b = [1,2])` retargeted b's MakeArrayV to a and b was never
  assigned (a wrong result the suite missed; probe-caught).
- R4 **Inc-dec with an IMPURE lvalue** (`a[f()]++` as value or statement:
  `incdec_lvalue_pure` declines a side-effecting index) — ✅ DONE
  (2026-07-14, maintainer-directed): the VALUE form is native via
  **`IncDecChainV`** + the serializable `Chunk::incdec_chains` pool: the
  lvalue decomposes into a root (container slot, or a compiled RVALUE
  temp whose VALUE seed keeps rvalue-ness — `mk()[0]++` still throws
  NotLValueEx) + member/subscript steps with each key compiled ONCE; the
  runtime walk is the shared StoreLValueChainV intermediate walk
  (`vm_chain_walk`), and the final step runs `vm_incdec_final`
  (eval.cpp) — IncDecExpr::do_eval's EXACT tiers: tier 2 = compound
  `±= 1` (flat/POD gated by the codegen-computed `allow_flat`/`allow_pod`
  = no_side_effects(final base), the tree-walker's own AST-shape gate)
  then old = new ∓ 1 with no re-read; tier 3 = the checked RMW. Probed
  byte-identical across flat/dict/dyn/nested/member/rvalue-root shapes
  (incl. the POD-member NotLValue and general-2D NotLValue corners).
  A follow-up closed the LAST real-code EvalStmt emitter found while
  CONFIRMING the zero-fallback claim: a nested named func/struct decl
  inside a LOOP/IF body (a scoped global) failed compile_scalar_body
  and dropped the whole loop to EvalStmt — now lowered by the shared
  emit_func_decl/emit_struct_decl (gen_stmt's top-level lowering,
  factored; a loop-body decl re-binds per iteration, as the
  tree-walker re-evals it). After it: the seven remaining EvalStmt
  emit sites are all whole-statement DECLINE NETS whose only known
  script-legal trigger is dev-only show() (harness-only); pinned by
  the reshaped codegen tests (nested-decl loop = native; the net
  witnessed via a show()-bearing condition under the harness).

**Live statement roots:**
- R5 **Typed/const-target IdList destructure** (`int a; int b;
  a, b = src`) — ✅ DONE (2026-07-14): `try_multi_unpack` now accepts
  typed int/float targets; a per-target coerce vector (0/1/2 =
  none/int/float) lives in the serializable **`Chunk::unpack_coerce`**
  pool (parallel to `unpack_targets`, `Instr::b` indexes it) and the
  MultiUnpackV handler runs `vm_coerce_decl_num` per store (the throw
  stamps the loc side table — same TypeErrorEx + caret as the
  tree-walker). The literal-eliding path still declines coerced
  targets (they need the runtime widen), falling to MultiUnpackV.
- R6 **`show()` under the -rt harness** (g_dev_builtins_allowed=true, so
  the differential compiles it; a SCRIPT compile-rejects it) — the ONE
  sanctioned EvalStmt consumer. It never has th==i/f (returns str), so
  it can NEVER need EvalToSlot; a cond/`if` use falls to the statement
  catch-all.

**DEAD paths (proven; delete or assert):**
- D1 `d?.f++` in EVERY form — the inferencer rejects it (NullabilityEx:
  the `?.` result is always optional, even null-narrowed). The
  documented residue was stale.
- D2 The NON-scope-free Block/body: `scope_free = false` requires a
  slot-less decl, whose ONLY producer is `declare_masking`, whose ONLY
  caller is the non-global named-func walk — script-unreachable post-F2
  (capture-slotting removed the old capture reason; named funcs can't
  capture; nested named decls hoist to scoped globals). In a SCRIPT,
  EVERY block is scope_free → gen_stmt's Block fallback is dead AND
  Tier 3's "non-scope-free bodies" reduces to EXPRESSION-bodied
  functions only.
- D3 Foreach non-local loop vars (a foreach header always DECLARES fresh
  locals in a script; map vars are REPL-only, and the REPL never runs
  codegen).
- D4 compile_native_try's non-slot catch var (REPL-only).

**The op-removal end state (maintainer-directed, 2026-07-14):** after
R1-R5 land — (a) `EvalToSlot` becomes unreachable (its only feeders were
recursive arg/operand declines + AST builtins in scalar positions, none
of which survive) → DELETE the op + eval_to_temp; (b) `JumpIfFalse`
becomes unreachable (a cond that fails the boxed path no longer exists
except show-in-cond, which falls to the STATEMENT catch-all) → DELETE
the op + the Phase-1 gen_if/gen_while forms (an if/while whose native
form fails falls back WHOLE via the gen_stmt catch-all); (c) `EvalStmt`
remains as THE single fallback op, reachable only by show()-in-tests
(the REPL never compiles). Execution order: R1 coalesce → R2 chained
cmp → R3 assign-expr → R5 typed IdList → R4 impure-lvalue inc-dec →
the D deletions + op removal, each its own commit.

**✅ EXECUTED (2026-07-14).** R1/R2/R3/R5 landed (above); R4's VALUE
form stays a documented EvalStmt user (statement form native). The op
removal is DONE: **`EvalToSlot` + `JumpIfFalse` are DELETED** (opcodes,
handlers, disasm renders, `vm_eval_cond`, `eval_to_temp`, and the
Phase-1 `gen_if`/`gen_while` flatten forms). The new decline behavior:
an int/float builtin operand that can't lower **fails its expression**
(no per-operand fallback op); an `if` whose condition can't compile
fails `compile_native_if` → gen_stmt emits a **whole-if EvalStmt**; a
`while` that can't lower natively is a **whole-while EvalStmt**; an
uncompilable loop **init** (`emit_init` now returns bool) fails the
whole loop the same way. So `EvalStmt` is THE single fallback op, and
the only node-holding op left in any compiled chunk — reachable only by
show()-in-tests, the R4 value form, and future gaps. (SUPERSEDED
2026-07-14: R4-value is now native too — IncDecChainV, below — so the
residue is show()-in-tests + future gaps only.) Suite 1522/1522 +
differential 1366/1366 green on g++/clang debug, RECYCLE+ASan, and
release; bench/ + samples/ still lower with an EMPTY node_table. New
codegen-shape tests pin R1/R2/R3/R5 (jinn/munpack counters) and the
whole-statement fallback behavior (the R4-value probe).

### The wrong-result bug this audit caught (fixed, step 5 — recorded here
### because it is the method's poster child)

`try_native_for_range` compiled a non-trivial BOUND into its reserved temp
BEFORE emitting the init ops, but `ForRangeStmt::do_eval` (eval.cpp) runs
**init → bound → step**. With a side-effecting init that changes the bound:

    func drop(v) { pop(v); return 0; }
    var x = [1, 2, 3]; var s = 0;
    for (var i = drop(x); i < len(x); i++) s += 1;

the tree-walker (spec) reads `len(x)` AFTER the pop → 2 iterations; the VM
read 3 → **s == 3 vs 2, a silent wrong result**. The fix: `emit_init` runs
FIRST, then the bound temp, then the step temp (matching do_eval's order);
pinned by the "for-range: the init evaluates BEFORE the once-read bound"
test. The lesson for future ops: when an op caches a value the tree-walker
evaluates lazily/in-order, the CODEGEN EMISSION ORDER is the RUNTIME
EVALUATION ORDER — always check the do_eval it mirrors, side effects
included.

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
   def pools). **The foreach / array-read ops are now node-free** (2026-07-09,
   `e67b6b2`): `LoadElem*`'s OOB caret moved to the loc side table and its dead
   non-array `node->eval` else-branch (unreachable - `base_array` is proven)
   became an `InternalErrorEx` net; `ArrLen`/`DictIterInit` nulled; the rest are
   already free. The remaining `node` users are exactly two groups:
   - the **builtin call ops** (`CallBuiltinV`/`LV`/`LVElem` + `EmplaceStruct`):
     their args are ALREADY frame-sourced (AST-free); `node` survives only for
     the baked `func_v`/`func_lv` ptr + the **per-arg error caret** (from the
     args `ExprList`). Freeing them = the **builtin loc-handle refactor**: bake
     the func ptr into a pool + carry the arg locs in a loc-handle. Does NOT
     require touching the 84 `func_v` builtin signatures.
   - the **fallback ops** (`EvalStmt`/`EvalToSlot`/`JumpIfFalse`): they hold the
     node to re-enter `node->eval`. Reached in real code ONLY by: **exceptions**
     (try/catch/throw/rethrow), the reflection builtins **`show`** (renders the
     AST - inherently node-based) and non-folding **`type`/`typestr`**, and the
     **flat struct-array literal** `[P(a),P(b)]`.

   **KEY (corrects the old claim):** dropping the field does NOT require every
   construct to be native. A fallback op can hold its node as an **index into a
   `Chunk::ast_nodes` pool** (`const Construct*`, program-lifetime, like
   `closure_defs`) via a spare operand, and `EvalStmt` runs
   `chunk.ast_nodes[in.a.lit]->eval(ctx)`. So the node-drop is a MECHANICAL
   pool-migration (builtin loc-handle + a node pool for the fallbacks), doable
   INDEPENDENTLY of nativizing exceptions - the two are ~orthogonal (see the
   decision note at the bottom).

3. **Exceptions (X / P8) - the last construct-level fallback + a hard `.myv`
   PREREQUISITE.** `try`/`catch`/`throw` are `EvalStmt`; every `throw` is a C++
   throw (bench 42, 24.5x). Needs VM-level exception dispatch (a handler stack +
   a pending-exception jump). **Full design + increment breakdown:
   `plans/vm-exceptions.md`.** One bench (~5% geomean) but the single
   most-dramatic per-bench win + it unblocks the serializable-bytecode endgame.

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

5. **Part C - native-but-slow** (independent of fallbacks). **PARKED while we do
   the `.myv` endgame (exceptions first); full write-up + assessment in
   `plans/vm-optimizations-deferred.md`.** The remaining Part-C items are
   diminishing-returns / risky on WSL2 - the big wins (P1-P7 +
   the store residuals) are done. Item by item:
   - **C2 computed-goto** (`&&label` threading; the DIRECT fix for the
     2026-07-08 dispatch regression). The one real lever, BUT: a ~64-handler
     mechanical conversion (bug-risk, differential-gated), and its front-end
     benefit CANNOT be isolated on WSL2 (no PMU) - and modern ITTAGE predictors
     have shrunk the classic 10-20% win. Do as a MEASURED EXPERIMENT: implement
     behind `#if defined(__GNUC__)` (switch fallback for MSVC), verify the 1220
     differential, then keep ONLY if the `--vm` wall-clock geomean is
     neutral-or-better (WSL2 wall-clock IS reliable here - benches are
     consistent - even though the PMU isn't). Not a blind commit (see
     [[vm-dispatch-frontend-regression]]: "don't perturb layout blind").
   - **C3 builtin arg-view ABI.** Marginal: the CallBuiltinV arg copy is a stack
     `EvalValue` copy per arg (cheap for the scalar builtins that dominate hot
     loops; only non-trivial args pay a refcount bump), and avoiding it touches
     all **84** `func_v` signatures. Low value-to-churn. Its genuinely-valuable
     half - freeing the builtin ops' `node` - is the **builtin loc-handle
     refactor**, which belongs with the node-drop work (item 2), NOT here.
   - **C4 `ModConst`** (`x = x % C` immediate form). Adds a NEW op → per
     [[vm-dispatch-frontend-regression]] that risks a front-end regression that
     dwarfs the tiny operand-decode saving; the plan itself calls it "minor" -
     likely net-negative on a switch dispatch - **defer until AFTER C2** (comp.-
     goto makes the op-set size front-end-neutral), or skip.
   - **C1 typed reads** largely landed as the typed dict / struct / element read
     ops.

## Decision: the "big loc table" (drop `Instr::node`) vs exceptions (P8)

They are **~orthogonal** (see item 2's KEY correction) - neither blocks the
other - so pick by value/effort, not dependency:

- **Drop `Instr::node`** (mechanical, broad, LOWER risk). Remaining work: the
  builtin loc-handle refactor + a `Chunk::ast_nodes` pool so the fallback ops
  (`EvalStmt`/`EvalToSlot`/`JumpIfFalse`) reference their node by index. Then
  remove the 8-byte field → a smaller `Instr` (~12% if Instr≈64B) → a hotter
  instruction stream (better I-cache on EVERY dispatch-bound loop, the broad
  win). Differential-safe; no new runtime feature. Payoff is broad-but-small and
  measurable (Instr size + `--vm` geomean).
- **Exceptions (P8)** (a NEW runtime feature, HIGHER effort/novelty). VM-level
  exception dispatch (a handler stack + a pending-exception jump) replaces the
  `EvalStmt`-per-try/catch/throw + the per-`throw` C++ throw (~1.6µs each).
  Removes the last real EvalStmt CONSTRUCT and is the single most-dramatic
  per-bench win (42 is ~24x CPython) but only ~5% geomean (one bench).

**Recommendation:** exceptions FIRST - it's the higher-value, higher-novelty
work, it removes the last *construct-level* fallback (leaving only the
inherently-node-based `show`/`type`/flat-struct-lit for the node-drop to
pool-encode), and doing it first means the subsequent node-drop pool-migration
covers a strictly smaller/cleaner fallback set. The node-drop is then a
low-risk mechanical sweep to finish the AST-free endgame.

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
