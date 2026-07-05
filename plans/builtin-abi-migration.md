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
Three reasons a read-only builtin can't take the value ABI:
- **Unevaluated / node-property operand:** `defined` (must not eval an undefined
  name), `isconst`/`isconstdecl` (read the *node's* compile-time `is_const`, not
  the value), `type`/`decltype`/`typestr`/`kindstr` (unevaluated operand, C++
  `decltype`-style), `show` (renders the arg's optimized tree).
- **arg0 is an LVALUE, not a value:** `sort`/`rev_sort`/`reverse` read arg0's
  `LValue*` to write back the sorted/reversed array (slice write-back) and to
  sort a `const`'s copy (`is_const_var`). The value ABI RValue's it away. Pinned
  by "Sort on slice"/"Reverse slice"/"sort of a const" (they'd need a
  *const-capable lvalue ABI* — a follow-up, distinct from the mutating `func_lv`,
  which is non-const).
- **Order-dependent validation (lazy arg eval):** `map`/`filter` validate arg0
  (the function) and throw `TypeErrorEx` BEFORE evaluating arg1 (the container),
  so `map(5, undefined_var)` is a type error on the `5`, not
  `UndefinedVariableEx` on arg1. The eager value ABI evaluates arg1 first and
  changes the error. Pinned by "map()/filter() validates its function argument
  first". (The eager-eval error-order shift is otherwise ACCEPTED for the
  migrated builtins — only these two have a *tested* ordering contract.)

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

Built, `-rt` differential-green both engines (1305/1305 + 1156/1156),
RECYCLE+ASan clean.
`ArrHint`-carrying creators (`array`/`range`/`make_array`/`keys`/`values`) keep
reading the hint off `exprList` (func_v still gets it). Net effect: the VM's
`CallBuiltinV` reaches every read-only builtin; the residual `func` (old-ABI)
set shrinks to exactly the AST-keep list above (a deliberate, documented floor).
