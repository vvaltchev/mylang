# Builtin ABI migration (value ABI, `-vm`)

Finishing the builtin side of the "remove every `node->eval` fallback" directive
([[vm-endgame]], `plans/bytecode-vm.md`). Three `Builtin` ABIs (evalvalue.h),
one union so `Builtin` stays 2 pointers / `EvalValue` 32 bytes:

- **`func`** — the ORIGINAL: gets the unevaluated `ExprList`, self-evaluates.
  Always set (the tree-walker + const-eval entry). For a migrated builtin it is
  a generated adapter (`builtin_v_adapter` / `builtin_lv_adapter`) so both
  engines share one impl.
- **`func_v(ctx, exprList, const EvalValue *args, size_t n)`** — READ-ONLY: the
  VM pre-evaluates the args into a register run and passes them by value; the
  builtin uses `args[i]` (never `elems[i]->eval`), still has `exprList` for
  arity/loc/`ArrHint`. Dispatched natively by `CallBuiltinV`. `make_*_builtin_v`.
- **`func_lv(ctx, exprList, LValue *target)`** — MUTATING (arg0 an lvalue):
  `append`/`push`/`pop`/`insert`/`erase`/`intptr`. `CallBuiltinLV`. See
  `plans/mutating-builtins-native.md` (its Phase 2 = value-args-native + emplace,
  separate from this file).

## Migration = mechanical
Per builtin: signature `(ctx, exprList)` → `(ctx, exprList, const EvalValue
*args, size_t n)`; replace each `RValue(exprList->elems[i]->eval(ctx))` with
`args[i]` (already an RValue), keep `n`-based arity checks + `exprList` locs;
registration `make_const_builtin("x", builtin_x)` → `make_const_builtin_v<
builtin_x>("x")` (or `make_builtin_v` for a runtime builtin). A callback arg
(`sort`/`map`/`filter`/`find`) is now owned by `args[i]` for the whole call —
strictly SAFER than the old self-eval-temporary lifetime.

## MUST stay on `func` (do NOT migrate) — the documented floor
ONE reason a read-only builtin can't take the value ABI (the other two -
lvalue-arg0 and map/filter's order-dependence - were RESOLVED, see below):
- **Unevaluated / node-property operand:** `defined` (must not eval an undefined
  name), `isconst`/`isconstdecl` (read the *node's* compile-time `is_const`, not
  the value), `type`/`decltype`/`typestr`/`kindstr` (unevaluated operand, C++
  `decltype`-style), `show` (renders the arg's optimized tree). Inherently
  node-based - the FLOOR, freed only by the AST-free builtin loc handle
  (`vm-ast-free.md` Steps 1-3) which keeps them node-referencing but AST-free.

**Order-dependent (map/filter) — RESOLVED (2026-07-08), still on `func`.** They
validate arg0 (the function) and throw `TypeErrorEx` BEFORE evaluating arg1, so
`map(5, undefined_var)` is a type error on the `5`, not `UndefinedVariableEx` on
arg1 (pinned by "map()/filter() validates its function argument first"). The
eager value ABI (`func_v`) would evaluate arg1 first, so they KEEP `func` for
the tree-walker - but the VM lowers them natively WITHOUT that ABI: a
**`CheckFuncV`** (validate arg0, throw before arg1's code runs) + a
**`MapFilterV`** calling the shared **`vm_map_filter`** core (generic.cpp.h,
declared in eval.h; the SAME core `builtin_map`/`builtin_filter` now call).
`DirectBuiltinCallExpr::map_filter_kind` (set in devirtualize) drives the
codegen. So they are no longer a `-vm` fallback.

## To migrate (read-only), by file — perf-hot first
The bench laggards are builtin-dense (`40_math_builtins` was already migrated in
num.cpp.h; `34_sort_custom_cmp` 0.94x, `35_map_filter` 0.77x, `27_dict_keys_
values` 0.82x, `31_str_split_join` 0.81x still old-ABI). Order:

**DONE (2026-07-05) — 31 builtins migrated to `func_v`:**
1. **arr.cpp.h** — `array` `array_storage` `dynarray` `make_array` `top` `range`
   `sum` (NOT `sort`/`rev_sort`/`reverse` — lvalue arg0, see above).
2. **generic.cpp.h** — `clone` `hash` `deepclone` `find` `assert` `runtime`
   `ispure` `ispuredecl` (NOT `map`/`filter` — order-dependent, see above).
3. **dict.cpp.h** — `keys` `values` `kvpairs` `dict` `get` `get!` (helpers
   `dict_get_impl`/`dict_1arg_func` refactored to carry `args`/`n`).
4. **num.cpp.h** — `rand` `randf`. **types.cpp** — `exit`.
5. **reflect.cpp.h** — `signature` `specializations` `globals` `layout` `trace`
   `traceoff` `tracing` (NOT `show` — renders the arg's tree).

**DONE (2026-07-08) — the lvalue-arg0 holdouts `sort`/`rev_sort`/`reverse` via a
const-capable lvalue ABI.** `make_const_builtin_lv(name, func, func_lv)`
registers a CUSTOM `func` (`sort_arr`/`reverse_arr` — the tree-walker/const-eval
path, eval's arg0 as value-or-lvalue) + a `func_lv` (`sort_lv`/`reverse_lv` —
the VM's `CallBuiltinLV` path, handed arg0's slot `LValue*`), both delegating to
a shared `sort_core`/`reverse_core` (the const-copy + slice write-back keyed off
an `LValue*` param, null for a non-lvalue arg0). Added to
`is_lvalue_arg_builtin` so the resolver devirtualizes to `CallBuiltinLV` AND
AutoConst/the specializer treat arg0 as a write position (no unsound const
substitution). It stays `const` (folds a const-array sort at parse time); the
cmp arg self-evals off `exprList` like the tree-walker. NOT the generic
`make_builtin_lv` adapter (that passes a null target for a non-lvalue arg0,
which sort/reverse accept but the adapter would mishandle). 1355/1355 +
1204/1204, RECYCLE 2/2. `sort(a)`/`reverse(a)` are now `call.blt.lv`, so a
reverse-in-a-loop (`21_array_reverse`) goes fully native. Only `map`/`filter`
(order-dependent) + the AST builtins remain old-ABI.

Built, `-rt` differential-green both engines (1305/1305 + 1156/1156),
RECYCLE+ASan clean.
`ArrHint`-carrying creators (`array`/`range`/`make_array`/`keys`/`values`) keep
reading the hint off `exprList` (func_v still gets it). Net effect: the VM's
`CallBuiltinV` reaches every read-only builtin; the residual `func` (old-ABI)
set shrinks to exactly the AST-keep list above (a deliberate, documented floor).
