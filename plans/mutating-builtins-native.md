# Native mutating builtins (VM)

Making the mutating builtins (`append`/`push`/`pop`/`insert`/`erase`/`intptr`)
run natively under `-vm` instead of the whole-call `node->eval` fallback. They
were the last builtin group on the fallback because their arg0 is an **lvalue**
(a mutable reference to the caller's container), which the value ABI destroys by
pre-evaluating.

See CLAUDE.md "The value & type model" (Builtin ABI) and `plans/bytecode-vm.md`.

## Phase 1 — DONE (commit `3e94b5b`)

The **lvalue ABI**: `Builtin::func_lv(ctx, exprList, LValue* target)`, sharing
the `func_v` UNION (mutually exclusive per builtin, discriminated by
`DirectBuiltinCallExpr::lvalue_arg0`, so `Builtin` stays 2 pointers / `EvalValue`
32). The builtin gets arg0 as an `LValue*` and **self-evaluates its remaining
args** (so `append` keeps its construct-in-place fast path, which needs the arg
node). `make_builtin_lv` registers `builtin_lv_adapter` as `func` (the
tree-walker / const-eval entry — evals arg0 to an lvalue, passes it), so both
engines share one impl. `CallBuiltinLV` forms arg0's `LValue*` straight from its
slot table by kind (local/global/capture; a not-yet-defined global → null target
→ `NotLValueEx`, matching `Identifier::do_eval`) and calls `func_lv`. Emitted
(`try_native_mutating_builtin`) only when arg0 is a **slotted identifier** — the
common `append(a, x)` form.

**Residuals left after Phase 1:**
1. A subscript/member lvalue **target** (`append(a[i], x)`, `append(d[k], x)`,
   `append(s.f, x)`) stays an `EvalStmt` fallback.
2. The **value args** are still self-evaluated (a contained `node->eval`), not
   passed natively — so `append`/`insert`/`erase` are native *dispatch + arg0*
   but not `node->eval`-free.

## Phase 2

Goal: remove BOTH residuals — the mutating builtins reach **zero `node->eval`**.

### 2a. Value args passed natively — DONE (2026-07-05)

`func_lv` extended to
`func_lv(ctx, exprList, LValue* target, const EvalValue* rest, size_t n_rest)`.
A **rest-native** builtin gets its value args (1..n) pre-evaluated in `rest`;
a **self-eval** one gets `rest == nullptr` and reads the node. Only **`insert`
(2 value args) and `erase` (1)** actually needed it — `pop`/`intptr` take arg0
only (no value args, already native), and `append`/`push` MUST stay self-eval
for construct-in-place (2b). So: `insert`/`erase` register via
`make_builtin_lv_v` (a second lvalue adapter, `builtin_lv_v_adapter`) and set
`DirectBuiltinCallExpr::lvalue_rest_native`; the VM's `CallBuiltinLV` compiles
their value args into a register run (`emit_args_range(..., start=1)`, base in
`Instr::b`) and `vm_call_builtin_lv_rest` (cold, ML_NOINLINE, stack ≤8 / heap
>8) copies them by value — arg0's `LValue*` is still formed from the slot table.
`insert`/`erase` now `-vd` as `load r,#i; load r,#v; call.blt.lv` with NO
`eval.stmt`. Both engines share `builtin_lv_v_adapter` (arg0 first, then the
rest — a pure slot ref, so the VM's rest-first order is observationally equal).
Verified 1305/1305 + 1156/1156 (incl. a side-effecting value arg), RECYCLE+ASan.

### 2b. `emplace` fusion — DONE (2026-07-05)

Landed via **recognition fork (a)** — codegen pattern match, VM-only, no AST
rewrite (the tree-walker keeps its own construct-in-place). The inferencer
stamps a POD struct construction with `CallExpr::vm_struct_ctor_def`
(`check_struct_construction`; transferred through the devirt swap). The codegen
(`try_native_mutating_builtin`) recognizes append/push (a self-eval lvalue
builtin, 2 args) whose arg1 is such a ctor, compiles the ctor's field-arg VALUES
into a register run (`emit_args_range`), and emits **`EmplaceStruct`** (arg0's
`LValue*` by slot kind + run base in `b`). `vm_emplace_struct` (eval.cpp,
declared in eval.h) does the FAST path — coerce the values straight into the
flat `array<Struct>`'s bytes (no temp `StructObject`, mirroring
`try_construct_into_struct_array`) — and a FALLBACK for a non-flat target
(`array<dyn>` holding structs: build the `StructObject` + general append). COW
(slice-clone), const/readonly checks, and float-field coercion match the
tree-walker byte-for-byte (differential-verified). `append(a, Point(5,6))` now
`-vd`s as `emplace r = append(&a <- Point(r1, r2))`; a plain `append(a, x)`
stays self-eval (construct-in-place for a Ctor, plain append otherwise).
Verified 1305/1305 + 1156/1156 (flat / dyn-fallback / float-coerce / slice-COW),
RECYCLE+ASan 3/3, `58_structs` 0.25x (~4x CPython), geomean ~4.1x.

(Original design below, for the record.)

This is the piece that lets `append`'s value arg be pre-evaluated WITHOUT losing
construct-in-place (the reason Phase 1 kept self-eval). The construct-in-place
does NOT actually need the ctor node: the **struct type is recoverable from the
target array** (a flat `array<Point>` carries its `StructTypeDef`), and the
constructor's arguments can be **pre-evaluated values**. So

```
append(pts, Point(i, i*2))   ->   emplace(pts, i, i*2)
```

- The codegen recognizes `append(target, Ctor(args))` where the inferencer
  proved `target` is `array<Struct>` and `Ctor` builds that `Struct`. It
  compiles the CTOR's arg exprs into the register run and emits an
  **`EmplaceStruct`** op (target lvalue + arg run + n).
- `EmplaceStruct` reads the target array's element `def`, coerces the value args
  field-by-field straight into the array's byte buffer (reuse `pod_store_field`
  / the field-coercion in `try_construct_into_struct_array`, but driven by
  VALUES, not the ctor node). **No temp `StructObject`** — so it is actually
  FASTER than today's tree-walker (which builds a temp, then byte-copies).
- A plain-value `append(a, x)` compiles `x` as a value and uses the normal
  append path (2a).

**Recognition fork (decide before starting):**
- (a) **codegen-only pattern match** — simplest; no AST rewrite; VM-only.
  *Recommended first.*
- (b) **inferencer rewrite** of the call to an internal `emplace` — cleaner, and
  the TREE-WALKER would share the win (drop its temp `StructObject` too), but
  touches more (a new internal builtin + the rewrite pass). Do after (a) if we
  want the tree-walker to benefit.

### 2c. Subscript/member lvalue targets

The other Phase 1 residual (independent of 2a/2b): `append(a[i], x)` etc. Needs a
native "eval arg0 to an element/field `LValue*`" — a `LoadElemLValue` /
`LoadMemberLValue` that produces the element/field lvalue (reusing the
StoreElem COW machinery: appending to `a[i]` must clone `a` if aliased). Until
then these stay `EvalToSlot` (correct, just not native). The shape test in
`vm_codegen_shapes` deliberately uses `append(a[0], i)` as its stable fallback —
update it when 2c lands.

### Verification bar (per CLAUDE.md)
Differential-green under both engines; RECYCLE+ASan clean; `bench/58_structs`
(the `append(pts, Point(...))` builder) must not regress and should IMPROVE once
2b drops the temp `StructObject`; cachegrind the append-heavy benches
(`13_array_append`, `58_structs`) `-vm` vs the Phase-1 baseline.
