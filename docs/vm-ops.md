MyLang — the VM op inventory
============================

What the bytecode VM lowers natively, construct by construct: which
opcode each source shape becomes, what its operands mean, which pool
carries its caret, and which shapes deliberately decline to a slower
tier. Also the side tables (locs, inline_ctxs/inline_frames) and the
`-vd` dump's contents.

The AUTHORITATIVE list of opcodes is `ML_FOR_EACH_OPCODE` in
`src/bytecode.h` — this file explains what they are FOR. Read the entry
for a construct before changing how it lowers, and read
`docs/jit-optimizations.md` for how the native tier then emits it.

Two things here are load-bearing enough that CLAUDE.md states them too,
and they are the reason this file has to stay accurate:

  * the codegen is NO-FAIL. Every statement and expression lowers, or
    the compiler THROWS `NotLoweredEx` — there is no fallback opcode and
    no path back into the AST. A gap is a loud compile refusal, never a
    silent tree-walk.
  * a NEW op that writes a frame slot must join `visit_use_def` AND be
    classified in `op_writes_scalar`; one with a pc field must join
    `visit_pc_fields`. Those tables are audited and several consumers
    read them at different pipeline stages.
  * a NEW op must ALSO be classified in `verify_chunk` (codegen.cpp) —
    the post-load verifier that bounds every operand against the table it
    indexes. Its switch has NO `default`, so the build FAILS until the op
    is classified; that is deliberate, because a stale entry there means
    silently accepting an unchecked operand from a `.myv` file. Say what
    each field IS: a frame slot, a run window, a pc, a pool index, an
    iterator or region id, a struct field. Anything the op dereferences
    (a `struct_defs` / `closure_defs` entry) also needs a non-null check.

KEEP IT IN SYNC — a change to the VM's op set updates this file in the
same commit, per CLAUDE.md's doc-sync rule.

**THE OP INVENTORY.** What follows is what the VM lowers natively today - the
whole language, since the codegen is no-fail (see the ZERO-AST rule above).
Resolved-local int/float/mixed scalar loops run native at top level, inside
function bodies, and NESTED — nested loops + `if` in a
loop body compile directly into the chunk; array element read/write `a[i]` /
`a[i]=v` / `a[i][j]` (and a subscript **inc-dec** `a[i]++`/`d[k]++`/`a[i]--` →
`StoreElemInt`/`Float` with a constant 1 for a flat array, or a `DictStore` with
a boxed 1 + the compound op for a dict — `== x[k] += 1`, the subscript's loc for
its caret) and a scalar builtin/call in an expression are native. The subscript
STORE ops **`DictStore`/`StoreElemValue`** are **AST-free**:
`vm_subscript_store`
(the shared `Type::subscript(for_write)` + `slot_rmw`) handles ANY base type, so
the `is<Dict>`/`is<Array>` guard + `node->eval` fallback were dropped and its
not-an-lvalue caret is now a `Loc` pair (from the loc side table, recorded by
`extract_locs` from `node` = the `Subscript`), not an `Expr14`-cast node.
A **boxed (dyn/string) value tier** covers scalar `dyn`/string
expressions — assign / compound-assign / comparison / logical `&&`/`||` over
locals, globals, captures, builtins, literals, subscripts (`a[i]` via the
runtime `Type::subscript`), members (`obj.f` / `d.k` via a shared
`member_read` factored out of `MemberExpr::do_eval`), and **slices**
(`a[i:j]` / `s[i:j]` → **`SliceV`**, via the runtime `base.get_type()->slice()`
— the same COW-registered sub-view path as `Slice::do_eval`, absent bounds
passed as `none`; ~2x on `bench/15`/`16` array-slice, `29` fully native) —
and **`foreach` over an array** lowers to a counted loop (snapshot + `ArrLen`
+ a per-element load + the
fused `ForLoopStep`, −64% instructions): a flat `array<int>`/`array<float>`
reads the raw scalar (`LoadElemInt/Float`, stamped `ForeachStmt::elem_th`), and
a **GENERAL element (`array<str>` / `array<array>` / `array<dict>` /
`array<dyn>`)** binds the element's existing `EvalValue` into the loop var via
**`LoadElemValue`** — **box-free (no box/unbox)**, matching the tree-walker's
general `elem = view[i].get()`, ~1.7x on a general-array foreach loop; the
inferencer stamps `ForeachStmt::container_is_array`. Both the **single-var**
(`foreach (e in a)`) and the **INDEXED 2-var** (`foreach (i, e in indexed a)`)
forms are native: for indexed, the index var IS the loop counter (the body
reads it) and the element loads into the 2nd var. A flat **`array<bool>`** binds
each element as a REAL bool (not 0/1) via **`LoadElemBool`**
(`ForeachStmt::elem_is_bool`), so `print(x)`/`str(x)` show `true`/`false` and
`x == true` holds — matching `arr_elem_boxed`'s bool case. A **foreach over a
proven STRING**
(`ForeachStmt::container_is_str`) is the same counted-loop shape with two string
ops: **`StrLen`** (the char count
bound, once) and **`LoadStrChar`** (bind char i as a fresh 1-char string, box-
free — matching `SharedStr(string(&view[i], 1))`); single-var and indexed 2-var
both native (`str.len`/`load.strchar` in `-vd`). Neither op can throw (i is
loop-bounded), so both are node/loc-free. A **flat `array<PodStruct>` foreach**
whose body reads the loop var
ONLY as SCALAR FIELDS (`p.x`) is native via a **DIRECT read**: the loop var is
NEVER materialized — the counted loop runs and each `p.field` compiles to
**`LoadStructFieldInt`/`LoadStructFieldFloat`**, a scalar read STRAIGHT from the
array element's bytes (`vm_struct_field_int/float`), skipping the per-iteration
`StructObject` + `memcpy` the tree-walker's reused-object foreach pays (**~4x**
on `65_struct_field_sum`). The inferencer stamps
`ForeachStmt::container_struct_def`; the codegen's `struct_fe_body_ok` proves
every loop-var use is a scalar-field READ (a whole-`p` use or a `p.field` WRITE
— which must NOT hit the array, `p` is a copy — falls back to the tree-walker's
reused-`StructObject` bind), and a per-body `sfe` mapping makes
`compile_int/float_expr` emit the direct read for `p.field`. The
**STRICT-UNPACK**
`foreach (x, y in
pairs)` over a proven `array<array<int>>` / `array<array<float>>` (flat
sub-arrays) is native too: the outer array iterates counted, and per element a
**`UnpackElemInt`/`UnpackElemFloat`** op reads `pairs[i]` (a general element = a
flat sub-array), strict-checks its length == the loop-var count `N`, and writes
its `N` scalars into the consecutive loop-var slots (`base..+N-1`) — matching
`do_iter`'s strict destructure (same two `TypeErrorEx`s, same container loc, via
the loc side table so the op is `node`-free). **Storage-kind guarded:** the
BOX-FREE raw read (`flat_ints`/`flat_floats`) fires only when the sub-array's
`skind()` actually IS that flat kind; ELSE (a MIXED-numeric literal like
`[int, float]`, which inference types `array<float>` by int|float join but
builds GENERAL storage, or a flat array of the OTHER scalar kind) it binds each
element's ACTUAL boxed value via `vm_arr_elem` — so an int stays an int under
`UnpackElemFloat`, byte-identical to `bind_loop_var` (blindly reading
`flat_floats()` on the general sub-array asserted/crashed — a fixed bug).
Stamped by the inferencer as `ForeachStmt::unpack_elem_th` (i/f). A
**general / dyn / str / mixed** sub-array (`array<array<dyn>>`,
`array<array<str>>`) — where the flat scalar path doesn't apply — is the
**`UnpackElemValue`** variant (`ForeachStmt::unpack_elem_value`): same op shape,
each element binds its boxed value via `vm_arr_elem`. This variant ALSO carries
the **`indexed` unpack** `foreach (i, name, price in indexed products)`
(shopping's F-2b shape): the index var is the loop counter and the unpack
targets follow it (`unpack_base = base + 1`, width `N - 1`). The inferencer's
`accumulate_foreach` was fixed to type an indexed 3+var loop's targets as the
sub-array's ELEMENT type (`dyn`/`str`), not the whole sub-array — matching
`do_iter` (which destructures the element), a latent front-end bug shopping
survived only because it used the vars in lenient builtins (`rpad`/`str`), never
arithmetic. A **`_` placeholder or a non-consecutive slot layout** (the loop
vars don't land in one contiguous run — a `_` gets NO slot) takes the
**`UnpackElemTargets`** variant: same op shape, but the per-position target
slots live in the **`Chunk::unpack_targets`** pool (`-1` for a `_`, which is
skipped) and each element binds box-free via `vm_arr_elem` (flat or general —
so it covers int/float/str/dyn sub-arrays uniformly). Handles the non-indexed
`foreach (a, _, c in pairs)` AND the indexed `foreach (i, _, v in indexed
rows)`. A non-local target still falls back. `20_foreach_unpack` (flat) 0.80x,
`75_indexed_unpack` (indexed str) 0.71x vs the tree-walker.
**Dict `foreach (k, v in d)` / `foreach (k in d)`** is native via a **LIVE
dict iterator** — a dict has no O(1) index, so it is NOT the counted-loop
but a while-shaped loop over two ops: **`DictIterInit`** pins the dict
(an `intrusive_ptr` copy → alive for the loop, matching the tree-walker's
lifetime-extended `cval`) and sets the `unordered_map` iterator to `begin()`;
**`DictIterNext`** tests it (→ end_pc on end), binds the key (and value,
`.put(it->first)`/`.put(it->second.get())` — like the array general case), and
`++`s. The per-loop iterator STATE lives in a **`vm_run_chunk`-local
`std::vector<DictIterState>`** (`{intrusive_ptr<DictObject> dict; iterator;}`)
sized by **`Chunk::n_dict_iters`** and indexed by a codegen-assigned `iter_id`
(monotonic, never reset → nested/sequential dict foreachs get distinct slots); a
mid-loop `return`/exception releases it when the frame unwinds — no cleanup op.
Advance is BEFORE the body: the visited sequence is identical to the
tree-walker's range-for, and the only difference (`++it` timing) is observable
only under mutation-during-iteration, which is UB in both engines (dicts don't
COW, so a body write hits the same map either way — a snapshot would DIVERGE, so
we DON'T snapshot). The inferencer stamps `ForeachStmt::container_is_dict` for a
1/2-var (key[+value]) loop over a proven `Dict` type — and the **INDEXED** form
`foreach (i, k[, v] in indexed d)` too: `ids[0]` is an int index counter
(`LoadImmInt 0` before the loop, `IntBin += 1` at each `continue` point, so it
holds the iteration number during the body, matching `do_iter`), and the
key/value follow it. Fixing this also fixed a **latent front-end mis-typing**:
`accumulate_foreach` typed an indexed dict's two targets as a destructured
element (both the KEY type), but `do_iter` binds `ids[1]=key`, `ids[2]=value`
(count==2) — so `v` was wrongly `str` for a `dict<str,int>`; now key + value
are typed distinctly (the tree-walker hid it by binding dynamically). A `dyn`
container falls back; `_`/keys-only bind a slot of `-1` (skip). ~1.5x on
`62_dict_word_count`
(0.71x→0.64x vs the tree-walker).
**A `foreach (<ids> in [indexed] <dyn>)`** — the
container's static type is `dyn`, so array-vs-dict can't be proven — is native
via a runtime-dispatching LIVE iterator, **`ForeachDynInit`**/**`ForeachDynNext`**
(like the dict pair, over a **`Chunk::n_dyn_iters`** `DynIterState` pool),
**GENERAL over the id list**: any var count, the `indexed` form, and `_`
placeholders. Init
pins the container, records the loop shape (`in.a.lit` packs
`nvars | indexed << 8`; the per-var frame slots — `-1` == `_` — are an
`unpack_targets` pool entry, `in.b`), and chooses ONCE — an
array (`{idx, size}`) or a dict (`{it}`), else throws `TypeErrorEx` (loc side
table); Next binds BOX-FREE from the state and advances, exactly as
`do_iter`: `indexed` binds `targets[0]` = the iteration counter; an ARRAY
element binds a single remaining var (`vm_arr_elem` → `arr_elem_at`) or is
STRICT-unpacked into N remaining vars (the same non-array / wrong-length
`TypeErrorEx`s, container caret via the side table, recorded on the Next op);
a DICT binds key [, value [, `none`-padded further vars]] (`do_iter`'s
count-2 padding). The inferencer stamps `ForeachStmt::container_is_dyn`
for ANY foreach over a `Dyn` container (a non-local — global/capture — loop
var still falls back, rare). An `opt` container is compile-REJECTED
(`NullabilityEx`), so "unproven container" has no valid-script residue.
This is the **F-2a** win — a `dyn`-param function dispatched
indirectly (its param never gets a concrete container type, e.g. phonebook's
`cmd_view`) now iterates natively instead of an `EvalStmt` fallback.
**Related fix:** a 1-var `indexed` foreach (`foreach (i in indexed a)`) has
NO value var — `do_iter`'s single-bind read `ids->elems[1]` OUT OF BOUNDS
and ABORTED (container hardening; UB in a plain release); it now binds
nothing beyond the index, consistent with the dict path. **~1.8x
CPython** on `66_dyn_foreach` (single-var; VM 0.59x the tree-walker) and **~3x
CPython** on `74_dyn_foreach_kv` (2-var dict; VM 0.54x the tree-walker) — the
box-free bind is the win. **Lever 4 (2026-07-28, shape
specialization):** ForeachDynInit resolves a per-shape **`DynIterState::
next` FUNCTION POINTER** once - a single-var non-indexed array picks a
per-`skind` body (flat int/float/bool: raw scalar read + bind through the
baked `slot0`; general/strs/structs keep the per-element `arr_elem_at`
dispatch - strs may PROMOTE mid-loop), a non-indexed 1/2-var dict binds
key/value through baked slots, everything else (indexed / `_` / N-var
unpack) keeps the generic body - so the per-element Next stops re-reading
targets/shape/nvars. Kind-stable by construction (flat int/float/bool
never promotes) and every fast body still re-derefs the container per
element (growth during the loop behaves as before). NOTE a `var dyn a =
range(N)` DESTINATION is GENERAL storage (the ArrHint rule) -> the gen
body; the flat bodies serve dyn ALIASES of typed arrays. Execution-proven
per body (`g_dyn_foreach_fast[5]`, the `dyn_foreach_fast_shapes` test).
Measured (callgrind Ir): 74_dyn_foreach_kv **-10.3%** (wall -8.6%),
66_dyn_foreach -7.0% (wall ~flat - its time is the boxed body arith,
the #60/N7 arc); 20/26 (typed paths) neutral. **User-function calls** go
native via `CallV`: a call
proved a user function (`CallExpr::vm_direct_func`, a Func static type — not a
struct constructor / builtin) that devirtualized to a global slot evaluates its
args into a register run (a `VmArgs` view over the caller's frame slots — no
per-call allocation) and calls `vm_call_func` → `do_func_call` with the VALUES
(no `node->eval` of the call). An **INDIRECT call of a func VALUE** (a closure /
lambda / func-valued var — a plain `CallExpr` with `vm_direct_func` but no
global slot) goes native via **`CallValueV`**: the callee EXPRESSION is compiled
into a temp (callee-first, matching the tree-walker), then the args run, and the
op reads the temp's `FuncObject` and `vm_call_func`s it — so
`11_closure_counter` (1.09x → 1.00x, no longer a VM loss) and `12_higher_order`
(0.54x) go native. This covers both an EXPRESSION-position value call and a
**discarded call STATEMENT** `fn(args);` (F-3, phonebook's `cmdfunc(data)` — a
func picked from a dict/array and dispatched; `gen_stmt` + the loop-body stmt
compiler route a plain-`CallExpr` statement to `try_native_value_call` after the
`Direct{Call,BuiltinCall}Expr` handlers, its result discarded). Modest speed
(the call is `do_func_call`-bound, both engines), but it removes phonebook's last
live `EvalStmt` (`76_funcval_dispatch` VM 0.93x vs the tree-walker). A **baked const array/dict/struct literal** (a `LiteralObj` —
what a fully-const `var a = [1,2,3]` / `d = {}` folds to) materializes via
**`LoadLiteralObjV`**, which calls the shared **`eval_literal_obj`** (the
immutable-share vs fresh-mutable-clone logic + the general/flat_s `arr_hint`
cases, factored out of `LiteralObj::do_eval`) from a `Chunk::literal_objs` pool
entry — AST-free, byte-identical to the tree-walker. A **plain assignment to a
GLOBAL-table slot** `g = <expr>` (a top-level var a function reads — the write
counterpart of `LoadGlobalV`) goes native via **`StoreGlobalV`** (`target` = the
`GlobalFuncTable` slot, `a` = the value temp): it writes `gfuncs->slots[target]`
+ `defined[target]=1`, which for a plain assign is byte-identical to the
tree-walker's decl (bind + define) AND reassign (`slot_rmw(op==assign)` ==
`put(RValue)`). A **compound** `g += x` and **inc-dec** `g++`/`g--` (a global
statement, lowered to `g += 1`/`-= 1`) use the SAME op with `aop` = the base op
(`Op::invalid` == plain assign): it requires the slot already `defined` (else
`UndefinedVariableEx`) then copy-modify-stores via `num_bin_op`, identical to
`CompoundV` (the compound rhs is a boxed operand — a complex rhs falls back).
The
compound/inc-dec variant carries a `node` for its caret (div/undefined, via the
loc side table); the plain assign is node-free. Compile in `compile_boxed_stmt`
(the gate now also accepts a `SymKind::global` lvalue; a global `IncDecExpr`
statement is handled at its top). An **INT/FLOAT-TYPED** global (needs
`coerce_to_decl_type`'s numeric widen / dyn-narrow throw) and a `const` global
(must throw `CannotRebindConstEx`) fall back to `EvalStmt`; a **non-scalar-typed
global** (`array<int> g`, `str g`, …) is native (its coerce is a no-op — see the
typed-decl note below).
Script-only (no `SymKind::global` in the REPL), so never emitted there; **not**
a pure-target retarget candidate (it writes the table, not a temp). A closure
**CAPTURE write** `cap = v` / `cap += v` / `cap++` (a counter `start++`) is the
exact analog — **`StoreCaptureV`** writes `(*ctx->captures)[target]` with the
same plain/compound/inc-dec `aop` split, no defined check (a capture is always
defined - snapshot at closure creation). So a closure BODY is now fully native
(the capture write was its last `EvalStmt`); 0-bench (the `do_func_call` call
overhead, engine-neutral, dominates a counter loop) but it completes the
slot-write family (local / global / capture). An **array LITERAL**
`[a, b, ..]` whose elements aren't all const (a fully-const *scalar* one is a
baked `LoadConstV`) builds native via **`MakeArrayV`**: the element expressions
compile into a
register run (the same contiguous-run pattern native calls use for args, via
`emit_args_range`), and the op builds the array through the tree-walker's shared
**`build_array_from_values`** core — flat (int/float/bool) or general per the
`ArrHint` carried in `target2`, box-free, never throwing (so `node`-free) — and
is retargeted straight into the lvalue slot for `var a = [..]` (no `MoveV`). A
**flat STRUCT-array literal** `[P(a,b), P(c,d)]` (`ArrHint::flat_s`, F-4) lowers
to the **FUSED `MakeStructArrayV`** (`try_make_struct_array`) when every element
is a same-POD-struct ctor with all-scalar field args: the N structs' field args
compile INTERLEAVED into one run (struct i's field j at `base + i*M + j`,
`M = nfields`), and `vm_make_struct_array` coerces them STRAIGHT into a
contiguous flat byte buffer — **no intermediate `StructObject` per element** —
then builds the mode-5 flat array. This **BEATS** the tree-walker's
`LiteralArray::do_eval` (which allocates N `StructObject`s then packs them):
`77_struct_array_lit` VM **0.85x** vs the tree-walker (was ~1.2x SLOWER under the
earlier per-element `StructCtorV`+`MakeArrayV` lowering — the `EmplaceStruct`
pattern for a whole literal). A MIXED / nested-struct-field / non-scalar-arg
literal declines the fused op and falls to per-element `StructCtorV` +
`MakeArrayV` (`build_array_from_values` packs the run VALUE-DRIVEN, the def off
the first element), and a still-unliftable one to `EvalStmt`. The `StructCtorV`
arg gate (shared `is_typed_scalar_arg`) accepts a scalar LITERAL (not only a
`th`-stamped operand), so an auto-const-folded arg (`var a=1; [P(a,a)]`, whose
folded literal has no `th`) lowers too. So a per-iteration array literal
(`22_multi_assign` 1.05x→0.89x; matrix/sieve/wordcount) no longer falls back to
`EvalStmt`. A **dict LITERAL** `{k0: v0, ..}` is the twin **`MakeDictV`**: the
key/value pairs compile INTERLEAVED into the run (`[k0,v0,k1,v1,..]`, key at
even/value at odd), and the op builds via the shared **`build_dict_from_pairs`**
(which freezes each key). Both share **`compile_to_run_slot`** (factored out of
`emit_args_range`) to place each element in its run slot. Dict-literal loops
(`25_dict_member` 0.63x, `62_dict_word_count` 0.69x vs the tree-walker) go
native. A **CLOSURE** `func [caps] (params) {..}` in expression position (a
returned / var-bound / call-arg lambda, `id == null`) builds via
**`MakeClosureV`**: `make_intrusive<FuncObject>(def, &ctx)` snapshots the
captures from `ctx` — byte-identical to `FuncDeclStmt::do_eval` for a lambda —
where `def` is a program-lifetime **`FuncDescriptor*`** from a
`Chunk::closure_defs` pool (the `Instr` holds only the index, and the pool
holds NO `Construct*`; the ctor never throws for a resolved closure, so no
loc). A **top-level `func f(..){}` decl
STATEMENT** bound into a hoisted GLOBAL slot reuses the same op — `gen_stmt`
emits `MakeClosureV` (the `FuncObject`) + **`StoreGlobalV`** (write the slot +
mark `defined`), byte-identical to `FuncDeclStmt::do_eval`'s global-bind
`slots[slot] = LValue(func, false); defined = 1` (a capturing named func / the
REPL map falls back). So the **whole function/closure path is native** — decl
BIND, closure CREATE, capture read/write, indirect CALL, and the compiled body —
via `MakeClosureV` + `StoreGlobalV`/`StoreCaptureV` + `CallV`/`CallValueV`.
Removing the per-func-decl `EvalStmt` cut the bench-suite fallback count 54→24.
A **`const` DECL of an arr/dict/func kept as a runtime symbol** (`const x =
<LiteralObj>`; const SCALARS are inlined, so never here) goes native via
**`DeclConstV`**: materialize the rvalue then BIND the slot as a **const
`LValue`** (`LValue(v, true)`) — a LOCAL (`target2==0`) or GLOBAL (`==1`) slot.
Binding const (not a plain `put`) is what keeps a later rebind throwing
`CannotRebindConstEx` (a rebind, having no `pInConstDecl`, stays `EvalStmt` and
throws via the tree-walker). The codegen (`compile_boxed_stmt`) recognizes it by
`Expr14::fl & pInConstDecl` — which distinguishes a DECL from a REASSIGN (a
const reassign is a RUNTIME error, not caught at compile time, so the codegen
does see it and must leave it to the tree-walker).
A **`struct P {..}` decl** binds the same way — `gen_stmt` bakes the type
descriptor (a trivial `t_structtype` value holding the program-lifetime
`StructTypeDef*`) into the const pool -> **`LoadConstV` + `StoreGlobalV`**.
The tree-walker binds it `const`, but that flag is unobservable at runtime (a
reassign `P = x` is a compile-time `CannotRebindConstEx`, `isconst` folds), so a
plain `StoreGlobalV` is differential-identical (a REPL map-resident struct falls
back). A standalone POD construction `P(x, y)` builds via **`StructCtorV`**: the
field args compile into a register run, then `construct_struct_from_values`
coerces them into the POD bytes (the `StructTypeDef*` is a `Chunk::struct_defs`
index, so node-free). It's gated on **every arg a typed scalar** (`th==i/f`) —
the inferencer already rejected a non-fitting typed arg, so `coerce` can't throw
and no per-arg loc is needed; a **nested POD-struct-field arg** (`L(P(1,2),
P(3,4))`) is accepted too (`pod_ctor_arg_safe`: the nested `StructCtorV`
produces exactly that struct type, so the parent's `coerce` can't throw), while
a `dyn` arg falls back to the tree-walker, which reports the exact arg loc (the
`append`-fused ctor is `EmplaceStruct`). A **BOXED (non-POD) construction**
`B(a, x)` with runtime args — an `array`/`dyn`/`opt` field makes B boxed (a
const-arg one folds to a `LiteralObj`) — is **`StructCtorBoxedV`**
(`CallExpr::vm_struct_boxed_def`, copied onto the `DirectCallExpr` in
`devirtualize_calls`): the field args compile into a register run, then
`construct_struct_boxed_from_values` mirrors `construct_struct`'s boxed loop
(coerce + emplace, none-fill omitted trailing opt fields). Here a field coerce
CAN throw (a dyn-laundered wrong value), so the **per-arg carets** are pooled in
a new serializable `Chunk::boxed_ctors` (`{def, ArgLoc[]}`) and the throw
reports the offending arg's caret — byte-identical to the tree-walker, AST-free.
The POD path exposed a latent VM bug: `a[i] =
<struct>` into a **flat struct array** has no boxed element LValue, so the
general `StoreElemValue` path (`subscript(for_write)`) wrongly raised
`NotLValueEx`; `vm_subscript_store` now byte-stores a flat POD-struct element
directly (bounds/type-check/COW/`memcpy`), mirroring `try_flat_subscript_store`.

**The residual container-STORE family (all now native).** A **struct field
store** `s.f = v` / `s.f OP= v` → **`StoreMemberV`** (`vm_member_store`: a POD
field coerces + byte-stores, a boxed field takes the field LValue + `slot_rmw`),
gated on the inferencer's `MemberExpr::base_struct`; a **nested store**
`a[i][j] = v` → **`StoreElem2V`** (`vm_nested_subscript_store` reads `a[i]` as a
reference then stores `[j]` into it — a FLAT inner via the shared
`flat_store_core`, a GENERAL inner via the element LValue; COW writes back
through the inner element), the generic N-level `a[k0]..[kn] = v` →
**`StoreElemChainV`** (`vm_subscript_chain_store` over a key temp run). Both are
AST-free with **PER-STEP subscript carets** in the **`Chunk::chain_locs`** pool
(a `{Loc,Loc}` per step, inside-out; a pointer, deref only on the throw path so
the hot store stays cheap): an INTERMEDIATE `a[9]` OOB carets the inner
subscript, the FINAL store the outer — byte-identical to the tree-walker's
per-node stamp (this replaced an earlier single-outer-loc imprecision where an
inner throw showed the whole `a[i][j]` span). **A store's base may be a GLOBAL
or CAPTURE container**, not only a
frame local: `as_container_base` (codegen) returns a slot **KIND** (0 local / 1
global / 2 capture) which the store ops carry in `in.target`, and
`vm_store_base`
(vm.cpp) forms the base `LValue*` from the frame / the `GlobalFuncTable` /
`ctx->captures` (an undefined global → `UndefinedVariableEx`) — so `a[i]=v` /
`d[k]=v` / `s.f=v` targeting a top-level container a function reads, or a
captured one, go native. **`StoreElemValue` is the UNIVERSAL store**:
`vm_subscript_store` now handles a **flat scalar** base too (via the shared
`flat_store_core`, factored out of `try_flat_subscript_store` alongside
`flat_writable_array`), so it dispatches **flat / general / dict** at runtime
exactly like the tree-walker's `try_flat`→general. The codegen emits it as the
**catch-all** for any container-slot base a fast path didn't take — a proven
GENERAL array, a **DYN / captured / unproven** base, or a flat int array whose
index isn't int-compilable (the flat `StoreElemInt` path rolls back and falls
through) — while a **proven flat int/float array keeps its unboxed
`StoreElemInt`/`StoreElemFloat`** (the catch-all excludes `th==f && base_array`,
left to `compile_float_stmt`). `StoreElemInt`'s compound `aop` carries any
BASE op from `compound_assign_base` — the arith five plus the shift/bitwise
six (`a[i] <<= n` is flat too); the flat body dispatches all eleven, the
shift arms checking a negative count BEFORE the COW clone through the
loc-stamped `vm_store_throw_negshift`, exactly like div0 (and like the
tree-walker's `flat_store_core`, whose `apply_compound_op` throws before its
clone). `StoreElemFloat` still meets only the arith five: a shift/bitwise
compound on a proven-float element is compile-rejected upstream. A **GENERAL nested lvalue-chain store** mixing
MEMBER and SUBSCRIPT steps (`a[i].f=v`, `q.p.x=v`, `s.f[i]=v`, `d.a[0].f=v`) →
**`StoreLValueChainV`** (`try_native_chain_store` decomposes the lvalue into a
slotted base + a `Chunk::chain_steps` list — a member is a `member_keys` pool
index, a subscript a pre-evaluated key temp, each with its own node loc). The
runtime walk (`vm_chain_lvalue_store_op`) carries `cur` as EITHER an `LValue*`
ref OR a plain VALUE — exactly the tree-walker's chained `do_eval`, where an
immutable intermediate (a POD field, a readonly instance) is a value READ
(`member_read_core`) the walk continues on, only failing `NotLValueEx` at the
FINAL store (so `q.p.x` on nested-POD carets the whole lvalue, not the inner
step). The final step dispatches: a struct → `vm_member_store` (POD byte / boxed
field), a **DICT member `d.f=v` → `vm_subscript_store(memId)`** (== `d["f"]=v`,
auto-vivify), a subscript → `vm_subscript_store`. Each step's throw uses ITS
node's loc (a subscript-only chain keeps the tuned `StoreElem2V`/
`StoreElemChainV`; a single `s.f`/`a[i]` keeps `StoreMemberV`/`StoreElemValue`).
So a member-in-the-middle nested store — which WORKS for boxed structs / dict
values, throws for POD — is native, byte-identical incl. carets. **P8 exceptions
are now fully native** (see
`plans/archived/vm-exceptions.md`): try/catch/finally + throw + rethrow + all
flow-crossing-try (incl. nested-finally chaining) are native ops. **G1
(2026-07-17): a VM-RAISED exception NEVER C++-throws, cross-frame included.**
`vm_raise` (the shared raise for `Throw`/`Reraise`/`Rethrow`/`EndFinally`'s
reraise and the IntBin/FloatBin div0 pair) dispatches to a SAME-frame handler
first (the un-cold fast path — routing it through the cold walk cost a
measured +12% on 42_exceptions), else runs the native FRAME WALK
(`vm_unwind_walk`) directly: pop in-VM records (capturing their backtrace
frames), dispatch at the first frame with a handler (the caller refreshes the
cached `code` pointer), or convert to the `g_vm_exc_pending` SIGNAL at the
activation's BOUNDARY record — pointer work end to end, no landing pad, no
exception clone (69_exc_crossframe 0.564x VM-wall, my/py 1.43x → **0.80x** —
the last CPython-losing bench now wins). Backtraces are byte-identical,
inclusive of INLINED virtual frames (`Chunk::inline_ctxs`, flushed by
`vm_flush_inline`). The boundary catch remains ONLY for the TYPE-SYSTEM C++
throws the VM can't pre-detect (OOB / KeyNotFound / a boxed `TypeErrorEx`):
a same-frame catch of those is native (boundary dispatch), a cross-frame one
pays one landing-pad to the boundary before the walk takes over.
**#74 increment 1 (2026-07-28) - the RTTI-free catch MATCHER:**
`RuntimeException` gained two cheap virtuals - `match_name()` (the
catch-matching name: a user struct exception's type name, else the
built-in `name`) and `is_exception_object()` (true only on
`ExceptionObjectTempl`, so callers may static_cast) - replacing the
`dynamic_cast<ExceptionObject*>` the VM's CatchTest matcher
(vm_exc_name/vm_catch_bind_val) and the tree-walker's do_catch ran per
caught exception: the RTTI cast measured ~218 Ir per call on the
multiple-inheritance ExceptionObjectTempl graph (~25% of
70_exc_runtime_error). Measured: 70_exc **-32.0%** Ir (155.7M ->
105.9M); the remaining residue is the interpreted catch-body resume
(vm_dispatch 13% - catch targets are excluded from per-pc entry stubs),
the name memcmp (~7%), and the per-throw pooled alloc + raise walk.
**#74 increment 2 - the NATIVE catch body:** a CatchTest's TARGET (the
catch body) is an ordinary RESUME - it joined the per-pc entry set and
its target field routes through entry_remap, so a matched catch enters
the fragment at the body instead of interpreting to the back edge. Only
PushHandler's target (the MATCHER pc, an exit-at-op native) keeps the
exclusion. Measured: 70_exc a further -4.5% Ir (vm_dispatch self -24%),
42_exceptions -2.0%, 69/71/72 neutral.
**#74 increment 3 - the INTERNED catch matcher:** `match_uid()` (a per-
class lazy-static interned name via the DECL_RUNTIME_EX macro; an
ExceptionObject carries the thrown struct's already-interned def->name)
+ the DERIVED `Chunk::catch_uids` pool (interned twins of catch_types -
NOT primary serializable data, a .myv load re-interns) let CatchTest and
do_catch compare POINTERS - the per-match string_view(name) paid a
strlen + memcmp, ~13% of the catch bench. nullptr match_uid = the
string fallback (a subclass without the override). A user struct
NAMED like a builtin still matches identically (same canonical intern).
Measured: 70_exc a further **-13.5%** Ir (string machinery 13% ->
0.1%), 42 -1.4%; cumulative #74: 155.7M -> 87.5M (**-43.8%**).
**#74 increment 4 - the lean per-throw lifecycle:** (1) the POOL's
single-element alloc/free fast paths moved INLINE into poolalloc.h
(the size at an ML_POOL_NEW_DELETE site is a compile-time constant, so
the size class folds and the hot path is a 4-5 instruction freelist
pop/push; the refill + out-of-range tiers stay out-of-line in
types.cpp - pool_alloc_slow/pool_free_slow - and the pool STATE stays
defined there, the global-mutable-state home; the ASan pass-through is
unchanged); (2) `vm_flush_inline` is an ML_ALWAYS_INLINE empty-gate
(`chunk.inline_ctxs.empty()` - the common no-inlining chunk skips the
call + pc search, ~27 Ir per raise) over the ML_NOINLINE walk.
Measured: 70_exc a further **-15.5%** Ir; the pool inlining also pays
on the dict-node paths - 23_dict_insert -4.1%, 67_make_dict -4.8%;
10/62 neutral. Cumulative #74: 155.7M -> 73.9M (**-52.5%**).
**#74 increment 5 (final) - the guard-free match_uid:** the macro's
function-local `static const UniqueId *u = get(...)` paid an
__cxa_guard acquire per catch (~18 Ir - thread-safe statics in a
single-threaded interpreter); a plain zero-init pointer + null-check
lazy init replaces it. 70_exc -1.6%. CAMPAIGN TOTAL: 155.7M -> 72.7M
(**-53.3%**); the residue is the CatchTest/EnterNative dispatch, the
same-frame vm_raise machinery (~40 Ir/raise), and the per-throw
ctor/dtor - each next step architectural for cold-path returns.

**#78 steps 1-2 (2026-07-30) - the DELETED-HANDLER-PC HANG + the
per-try-REGION exception state.** The catch-dispatch redesign began with
a ~40-test battery (the `catchd:` block) pinning dispatch-order
semantics, which immediately found FIVE pre-existing bugs. (1) A JIT
HANG shipped with the #56 final batch: a try/FINALLY with NO catch
clauses has no CatchTest keeping its run alive, so the region deletes
and the handler pc a raise dispatches to (PushHandler's target)
collapsed onto the head EnterNative - the throw resumed at the run
start and looped FOREVER. Fixed: such a target gets an interior entry
STUB (the post-call-resume machinery, generic) and PushHandler.target
routes through entry_remap. (2-4) The nested pend/exc CLOBBER class:
ONE per-record {exc, pend} pair served every try region of a frame, so
same-frame nesting clobbered it - a try/catch inside a FINALLY body let
the pending exception ESCAPE, a try/FINALLY inside a finally silently
LOST it, and a caught inner try corrupted what `rethrow` re-raises (the
tree-walker was immune: its state lives in TryCatchStmt::do_eval C++
locals, nested by recursion). Fixed by PER-TRY-REGION slots: each `try`
gets a chunk-static monotonic REGION ID (`Chunk::n_trys`, serialized -
myv v7) baked into PushHandler(a)/SetPend(a)/EndFinally(a)/CatchTest(b)/
Reraise(a)/Rethrow(a = the innermost in_catch try's region); the runtime
state is `act.pends[rec.pend_base + region]` (`VmPendState {exc, pend}`,
the dict_iters watermark pattern; `vm_dispatch_exc` PARKS the exception
in the handler's region slot - `VmHandler` grew to {catch_pc, region},
8 bytes, so the emitted M5b handler_base bake shifts by 3 now - M5b is
the fully-inline record push, docs/jit-optimizations.md). A
record's `pend_base` is VALID ONLY when run_chunk->n_trys != 0: the M5b
inline push DECLINES try-bearing callees (like the iter pools), so
try-free records may carry a stale pend_base - every reader (pend_at,
pop_window's trim) is gated on n_trys. The nested-rethrow semantics fell
out CORRECT for free (the outer region's slot still holds its exception
while an inner region traffics). (5) The finally-body FLOW spec was
pinned from the tree-walker: a flow signal raised INSIDE a finally body
REPLACES the pending action (`return`/`break` win, the exception is
abandoned). ONE deliberate residue, flagged as a DESIGN FORK, not fixed:
`rethrow` inside an inner TRY body within a catch body - the tree-walker
propagates from the OWNING try, the VM (like Python/C#) raises at the
rethrow SITE and the inner catch intercepts; maintainer to rule.

**#78 steps A-C - the HANDLER TABLE, and the raise path dispatching
from it.** The catch chain (an interpreted `CatchTest` per clause, each
re-deciding at runtime what the compiler already knew) is replaced by
COMPILE-TIME data: **`Chunk::handler_sites`**, one entry per try REGION
holding its clause list (`HandlerClause {types_idx, bind_slot,
body_pc}`), the shared `fin_pc` (-1 = none), and a **`has_rethrow`**
flag (does any catch body of this try contain a `rethrow`?). PRIMARY
data - step D deletes the chain it could be derived from - so it is
SERIALIZED (myv v8) and REMAPPED at every pc-moving transformation (the
peephole's threading + branch-target map + compaction prefix-sum, and
BOTH JIT remaps: `remap` for the container path, `entry_remap` for the
delete-originals rebuild, since a clause body is a RESUME pc).
`peephole_chunk` takes the chunk non-const for it. The net is
**`verify_handler_sites`** (ASSERTS-only, run after codegen and after
both remaps): it re-walks each PushHandler's interpreted chain and
asserts the table still describes it exactly - it fired on its first run
(the peephole DELETES the no-match `Jump` when the shared finally is the
next pc; the table was right, the checker's chain model was wrong).
**Step C flips the dispatch:** `vm_dispatch_exc` now owns the whole
same-frame decision - pop the handler, index `handler_sites[region]`,
run the shared `vm_catch_match` (step A, ML_ALWAYS_INLINE - left
out-of-line it cost +40 Ir per throw) over the clauses, bind, **park the
exception ONLY IF `has_rethrow`** (the park is an owning move + a later
free, and the overwhelmingly common catch does not rethrow), resume at
the winning `body_pc`; no match with a `fin_pc` parks a `reraise` and
resumes in the finally; else it keeps walking OUT to the next enclosing
handler before returning false to the frame walk. CatchTest/Reraise are
still emitted but never EXECUTED (step D deletes them). **The JIT entry
set is now sourced FROM THE TABLE** (every `body_pc` and every
`fin_pc`), not from CatchTest's target - without the `fin_pc` half a
throw resumed at a pc inside a DELETED run, collapsed onto its head
EnterNative: the step-1 hang from the other side. FORM: ONE
out-of-line function - inlining it at its 5 call sites (3 inside
`vm_dispatch`) measured **+3.6M Ir** on 42_exceptions, the loop-body
TEXT rule, and splitting a cold tail out of it +2.7M; both reverted.
**Step D DELETED both opcodes.** With them went PushHandler's target pc
(it carries only its region id - so it left `visit_pc_fields`,
`op_is_branch`/`branch_pc_target`, and its emit moved from emit_branch to
emit_op), `VmHandler`'s catch_pc (a bare 4-byte region id again: the
emitted inline push/pop step 4 bytes and the handler_base bake shifts by
2), the "no clause matched" bytecode exit (so a plain try/finally emits
ONE SetPend, not two), the step-1 PushHandler-target entry stub (the
handler-table entry loop is its general form), and
`verify_handler_sites`' table-vs-chain walk - rewritten as the REMAP net
it was really for (every table pc in range, every region resolvable, no
empty site). myv **v9** (deleting two opcodes renumbers the rest). **THE
LOAD-BEARING NEW ROOT: the peephole's reachability DFS seeds from the
handler table** - a catch body's only predecessor WAS the CatchTest
jumping to it, so without this the DFS calls every catch body dead and
deletes it. Structurally this is the goal: with no matcher pc to dispatch
into, a try/catch region DELETES like any other run -
70_exc_runtime_error's main chunk goes 35 ops -> **2**, 42_exceptions'
-> 3, and the corpus audit (`MYLANG_DELAUDIT=1` over bench/ + samples/)
goes from 9 kept runs to **1** - all 9 were the catch dispatch. **Step E
took it to ZERO:** EndFinally's cold RERAISE arm calls `jit_end_finally`
(the interpreted body's shared `vm_raise`), reporting like `jit_throw` -
0 dispatched (handler pc parked + returned as an external exit; the op
already ran, so it is a RESUME), 1 boundary, 2 conveyed, 3 nothing
pending - while the HOT normal arm stays the inline byte compare. Its
`inline_chain` argument is `inline_frame_at(old_pc)` resolved at COMPILE
time and stamped on the exception before the raise (a deleted run's pcs
collapse onto the head EnterNative, where the pc lookup cannot
discriminate). TRAP: the cold arm outruns a short jump (`emit_exc_stamp`
alone does), so the normal/nothing-pending jumps are rel32 - the first
build asserted in `Emitter::patch8`. Execution-proven by
`g_jit_end_finally_reraise` over all three outcomes plus a loop shape.
**#80 finished the set: `rethrow` is native too.** It was the LAST
jit-ineligible exception op, so it SPLIT the run it sat in and kept its
whole try region interpreted (measured: the same program is 24 ops / 4
fragments with a `rethrow`, 3 / 3 without). `jit_rethrow` is
jit_end_finally's shape plus the site caret restamp: take the caught
exception out of the ENCLOSING catch's region slot (guaranteed parked -
that is exactly what `has_rethrow` gates), stamp the RETHROW SITE's
caret, `vm_raise`; 0 dispatched / 1 boundary / 2 conveyed, never falls
through. Both the LocEntry and the inlined-at chain are BAKED at compile
time, since the interpreted op reads them via `loc_at(pc)` /
`inline_frame_at(pc)` and a deleted run's pcs collapse. Pinned by
`g_jit_rethrow_native` over the same three outcomes AND a CARET-PARITY
check (tw == vm, and the position must be the `rethrow` line): a wrong
bake would leave behavior correct and only move the error position,
which nothing else would catch. With this, EVERY exception construct -
`throw`, runtime errors, `try`/`catch` entry+matching, `finally`,
`rethrow` - is native. Two fixes landed from measurement:
**the FAST REJECT** - the frame WALK calls the dispatch once per POPPED
frame and almost none of those frames have a live try; pre-#78 the
dispatch inlined so the reject was free, and paying a call frame per
walked frame cost 162 Ir/frame = **+6.4% on 69_exc_crossframe**, fixed
by an ML_ALWAYS_INLINE emptiness wrapper over the ML_NOINLINE loop
(69: +6.37% -> **-2.36%**) - and **do/while**, since the wrapper already
proved the first iteration's condition (+6 Ir/throw otherwise).
PERF (all figures ASSERTS=0 - see THE ASSERTS=0 MEASUREMENT RULE; the
ASSERTS=1 readings this paragraph first carried were not extrapolable and
one flipped sign). End to end vs the pre-#78 baseline, AFTER the hot-path
fix below: 72_exc_finally **-1.28%**, 69_exc_crossframe **+0.30%**,
42_exceptions **+0.71%**, 70_exc_runtime_error **+2.04%** (they were
+2.83% and +9.47% before that fix). **THE FIX - `vm_dispatch_exc_hot`:**
an ASSERTS=0 profile of 70 (361 Ir per throw once the 4.13M startup floor
is subtracted) showed `vm_dispatch_exc_frame`'s own PROLOGUE+EPILOGUE at
**28 Ir/throw** (17 in `{`, 11 in `}`) against a body doing ~15 Ir of
real work - the frame was nearly twice the function. `vm_raise` now uses
an INLINED copy of the shared body while the boxed-throw sites inside
`vm_dispatch` keep the out-of-line one (inlining THERE measured +3.6M -
the loop-body text rule); vm_raise is reached from the throw HELPERS, not
the dispatch loop, so it costs no loop text. Worth **-6.79%** on 70 and
**-2.06%** on 42. Two profile findings that looked alarming were NOT
throw-path costs: `__dynamic_cast` and `__strcmp_avx2` fire 27k/71k times
against 200k throws - they are COMPILE-time (the RTTI-free matcher is
intact). The fragment round trip (39 Ir/throw; the fragment is entered
exactly once per throw) is now the largest single remaining item and
would need a direct fragment-to-fragment `jmp` on a dispatched throw -
never a `call`, which would nest a C frame per iteration and overflow.
The step-C projection (that deleting
the two dispatch-switch cases would return step 2's +36 Ir/throw) was
**WRONG** - it returned 0.5M of 7.2M - and is recorded as wrong. Split by
the JIT kill switch, the residual is: a DESIGN cost (+1.4% / +4.2% with
`-nj`) - `vm_dispatch_exc_frame` is 89-116 Ir/throw where the chain was
~50, ~32 of it NESTED-VECTOR addressing (a `vector<HandlerClause>` per
site, a `vector<const UniqueId *>` per clause: three indirection levels
per match) - **and flattening that into chunk-level arrays was TRIED
and is REFUSED: it measured +1.14% on 42_exceptions and +2.88% on
70_exc_runtime_error at `OPT=1 ASSERTS=0`. The three-hops premise is
false of the generated CODE — the inner vector's pointers live INSIDE
the site struct the dispatch already loaded, so the nested range-for
has no extra dependent load while the flat form must fetch
`handler_clauses.data()`, `clause_base` and `n_clauses`. Do not
re-attempt without reading
`plans/archived/vm-optimizations-rejected.md`** -
and a JIT-SHAPE cost (the rest), inherent to delete-originals on a
throw-EVERY-iteration loop, which now leaves and re-enters ONE big
fragment per iteration instead of stepping through several small ones.
Those two microbenches are that shape's worst case; 72 and 69, which do
real work per iteration, both improved.

A **multi-assign destructure of an array LITERAL** — `a, b, c = [e0, e1,
e2]` (an `Expr14` whose lvalue is an `IdList`) — is lowered by
**`try_multi_literal_store`** (codegen) NOT by building the array and unpacking,
but by compiling each element into a snapshot temp and **distributing** the
snapshots to the target slots — **eliminating the array alloc entirely** (the
real win; box-free for int/float elements). Snapshot-FIRST makes it swap-safe
(`a, b = [b, a]`), matching the tree-walker's build-then-bind. Only when the
rvalue is a `LiteralArray` of EXACTLY the target count and every target is a
real (non-`_`), resolved-local, non-const, non-coerced identifier; a `_`
placeholder, an arity mismatch (a runtime strict error), a non-literal array
(`a,b = f()`), or a typed/const target all fall back to the strict
`handle_single_expr14` `EvalStmt` (byte-identical under the differential).
A **SCALAR SPREAD** `a, b, c = <non-array scalar>` (the sibling
`try_multi_scalar_spread`) compiles the rvalue ONCE and **`MoveV`s (copy/alias)
it to every target** — no array, no runtime destructure-vs-spread branch. Gated
on a provably-non-array rvalue (a proven int/float `th`, or a scalar `Literal` —
int/bool/float/str/none, NOT `LiteralObj`); an array/dyn rvalue falls back to
the strict destructure. So `var a,b,c = 0` / `= "hi"` / `= x` (int `x`) are
native. **Effect: `22_multi_assign` 0.89x → 0.18x** (the per-iteration
array alloc was the whole cost). Every OTHER IdList destructure — an array
VALUE (`x, y, z = arr`, `= products[pnum]`), a const array literal (a folded
`LiteralObj`), a `dyn` rvalue — is the catch-all **`MultiUnpackV`**
(`try_multi_unpack`), tried after the two eliding paths: it compiles the rvalue
into a temp and the op runs the tree-walker's STRICT destructure — an array
value is length-checked and its elements distributed **box-free** via
`vm_arr_elem` (skind-dispatched: sound for a general/flat/dyn element), a
non-array SPREADs to every target. AST-free: the target slots (with `-1` for
`_`) live in the **`Chunk::unpack_targets`** pool, and the strict-length caret
in the loc side table records the enclosing `Expr14`'s span — matching the
tree-walker, whose loc-less IdList lvalue makes the error inherit the Expr14
loc via `Construct::eval`. So the WHOLE IdList branch is native (no residual
`EvalStmt`); a typed/const target still falls back. **Effect: `73_multi_unpack`
(the array-value form) 0.30x vs the tree-walker.** A
**`return <expr>;`** likewise lowers to a
`ReturnV` that compiles the return expression (so `return f(x)` → CallV) then
sets flow={ret,value} and stops the chunk. A **ternary VALUE** (`cond ? a : b`)
compiles to a branch producing one arm into a slot, and a **`CachedCallV`**
(a `CachedCallExpr`) routes through the caller frame's per-frame `PureCache` —
together these make a recursion-unroll return native, so **`fib` runs fully
native** (a plain CallV would BYPASS the cache and recompute the exponential).
A **read-only builtin** with the VALUE ABI (`Builtin::func_v` — args
pre-evaluated, no `node->eval`; set by `make_const_builtin_v`, which also
registers a generic adapter as the tree-walker's `func`) dispatches natively via
`CallBuiltinV`. **`func_v` is AST-FREE**: instead of an `ExprList *`, it takes an
**`ArgLocs`** (`evalvalue.h`) — the whole-args caret + per-arg carets
(`arg(i)->start/end`) + the array-repr `arr_hint`, i.e. EXACTLY the source-loc
data a builtin's error messages used to reach through the arg nodes, and nothing
executable. The tree-walker adapter builds it from the real `ExprList`
(`build_arglocs`, types.cpp); the VM builds it (`vm_build_arglocs`, vm.cpp) —
today from `dc->args` (the node), next from a serializable `Chunk` pool — so a
migrated builtin holds NO AST pointer, and its carets are byte-identical in both
engines. A **mutating builtin** (append/pop/insert/erase/intptr) uses the
**lvalue ABI** (`Builtin::func_lv` — a UNION with `func_v`, discriminated by
`DirectBuiltinCallExpr::lvalue_arg0`): it gets arg0 as an `LValue*` target,
dispatching via **`CallBuiltinLV`** when arg0 is a slotted identifier
(local/global/capture — the op forms the `LValue*` straight from the table). The
value args (1..n) — the `rest` args, i.e. the **TAIL ARGS BY VALUE** (everything
after the arg0 lvalue) — come in one of three forms, decided **PER-OP** (the VM
reads `in.b.is_lit` — a valid `b` = a compiled rest run = this op is rest-native
— NOT a per-builtin flag): (1) a **rest-native** builtin (`insert`/`erase`,
`make_builtin_lv_v` + `lvalue_rest_native`, ALWAYS) has them pre-evaluated in
`rest`/`n_rest` (the VM compiles a register run, base in `Instr::b`;
`vm_call_builtin_lv_rest` copies by value) — **zero `node->eval`**; (2) a
**rest-native-CAPABLE** builtin (`append`/`push`, `lvalue_rest_capable`) has its
single value arg pre-evaluated too, but only in the PLAIN case — the codegen
compiles the value into a rest run PER-OP and marks that op, while the ctor case
(`EmplaceStruct`) and the subscript-target case (`CallBuiltinLVElem`) stay
self-eval (see below); (3) a **self-eval** call (`rest == nullptr`) reads its
args off `exprList` — the tree-walker's append/push (so construct-in-place fires),
`pop`/`intptr` (no value args), `sort`/`reverse` (their cmp arg), and any
append/push op the codegen left self-eval (a ctor-fallthrough, a subscript
target). This **per-op** design is what lets `append`'s three call shapes coexist
(a plain `append(a, x)` is rest-native = no `node->eval`, `append(a, P(..))`
= `EmplaceStruct`, `append(a[i], x)` = `CallBuiltinLVElem`).
**`append(struct_arr, Ctor(args))` fuses to an
`EmplaceStruct` op (Phase 2b)**: the inferencer stamps a POD struct construction
with `CallExpr::vm_struct_ctor_def`; the codegen recognizes append/push of such
a ctor, compiles the ctor's field-arg VALUES into a register run, and emits
`EmplaceStruct` (arg0's `LValue*` by slot kind + the run base in `b`);
`vm_emplace_struct` (eval.cpp) coerces those values straight into the flat
`array<Struct>`'s bytes — **no temp `StructObject`** (a non-flat `array<dyn>`
target falls back to build+append, matching the tree-walker). So a
`append(pts, Point(i, i*2))` build loop is fully native. **`EmplaceStruct` is
AST-FREE** (the `Chunk::emplace_sites` pool: the ctor's POD def + the
container-arg caret + the per-field coerce carets + the callee name, indexed
by the kind-packed `Instr::a`; the whole-args caret rides the loc side
table), so a struct-append chunk holds no AST pointer at all.
An **AST builtin**
(defined/type/…, needs the arg node) keeps the union null (its call site
falls back whole) — EXCEPT `defined`, now fully lowered: only a bare
`Identifier`
can evaluate to the `UndefinedId` sentinel (`Identifier::do_eval` is its sole
producer), so `defined(<non-identifier expr>)` is exactly "evaluate the arg
(effects/throws included), then `true`" (`try_native_defined_expr`: compile
the arg + `LoadConstV true`, AST-free), and a wrong-arity `defined(a, b)` —
which throws BEFORE evaluating any arg — is a bare
`ThrowRuntimeV(bad_args)` → `InvalidNumberOfArgsEx` with the args caret.
With `try_fold_defined` (identifiers) + `DefinedGlobalV` (globals), every
`defined` form is now fold/native.
**The read-only builtin migration to `func_v` is
complete** (see `plans/archived/builtin-abi-migration.md`): every read-only builtin whose
args are plain values now dispatches via `CallBuiltinV`. **`sort`/`rev_sort`/
`reverse`** — whose arg0 is an `LValue` (slice write-back + a const's copy) —
migrated to a **const-capable lvalue ABI**: `make_const_builtin_lv` registers a
custom `func` (the tree-walker / const-eval path, `sort_arr`/`reverse_arr`,
eval's arg0 as value-or-lvalue) PLUS a `func_lv` (`sort_lv`/`reverse_lv`,
handed arg0's slot `LValue*` by the VM's `CallBuiltinLV`), sharing a
`sort_core`/`reverse_core` so both engines behave identically and a const-array
sort still folds at parse time. They're now in `is_lvalue_arg_builtin` (so the
VM devirtualizes to `CallBuiltinLV`, and AutoConst/the specializer correctly
treat arg0 as a write position — no unsound const substitution into a sort).
**`map`/`filter`** — which must validate arg0 (the function) and throw BEFORE
evaluating arg1 (a TESTED order the eager value ABI would break) — go native via
a two-op sequence in **`compile_boxed_expr`** (`try_native_map_filter`): compile
arg0 into a temp, **`CheckFuncV`** (throws with arg0's caret if it isn't a
function, BEFORE arg1's code runs), compile arg1, **`MapFilterV`** (calls the
shared **`vm_map_filter`** in generic.cpp.h, declared in eval.h — map builds a
fresh array, filter keeps truthy elements; array→array, dict→dict — the SAME
core the tree-walker's `builtin_map`/`builtin_filter` now call). The
devirtualize pass sets `DirectBuiltinCallExpr::map_filter_kind` from the callee
name. So the residual old-ABI (`func`, node-based) floor is now exactly ONE
principled group — the **AST builtins** (`defined`/`isconst`/`isconstdecl`/
`type`/`decltype`/`typestr`/`kindstr`/`show`: an unevaluated / node-property
operand, inherently node-based).
**A SUBSCRIPT lvalue target `append/push/pop(a[i], d[k])` goes native too
(Phase 2c, `CallBuiltinLVElem`)**: the codegen compiles the index and records
the base slot; the handler forms the element `LValue*` by calling the runtime
`Type::subscript(base, idx, for_write=false)` directly — the SAME COW the
tree-walker's `Subscript::do_eval` uses — then `func_lv`. Still fallbacks: a
MEMBER target (`append(s.f, x)`), `insert`/`erase` with a subscript target, a
NESTED base (`a[i][j]`), and struct construction. **THE DEFAULT
FLIPPED 2026-07-18**: a script/`-e` run executes on the VM (both documented
conditions long met — full parity + the VM ~2.2x the tree-walker on the
bench geomean, suite 5.0x CPython); `-tw` selects the tree-walker, `-vm` is
accepted as the explicit default (pre-flip scripts/CI), the two are
mutually exclusive. The REPL and parse-time const-eval remain tree-walker
by design (they need the AST). `tests/nested_fuzz.py`'s tw lane passes
`-tw` explicitly; `bench/run.py` runs the VM by default (`--tw` for the
tree-walker; `--vm --baseline <same post-flip binary>` still gates VM vs
tree-walker — the baseline now gets `-tw`). Implemented in its **own files** — `bytecode.h` (the `OpCode`
enum — named `OpCode`, since `Op` is already the operator enum in
`operators.h` — plus `Instr`/`Chunk`), `codegen.{h,cpp}` (`codegen_program`,
AST→`Chunk` lowering), `vm.{h,cpp}` (`vm_execute` + the `g_exec_engine`
harness switch) — never woven into `eval.cpp`. The conversion itself is
history (`plans/archived/bytecode-vm.md`); its scaffolding opcodes -
`EvalStmt`/`EvalExpr`, `JumpIfFalse`, `LoopBackEdge` - are all DELETED, and
that last deletion is why **the VM's only remaining FlowState use is
ReturnV's value hand-off to `do_func_call`**: with no fallback body left,
nothing inside a chunk can set a brk/cont/ret FlowState
(plans/archived/vm-native-call-stack.md Phase A).
The VM is a **REGISTER machine over the frame slots** (the VM's registers
ARE the resolved-local slots — NO value stack), with fused superinstructions:
`IntBin` (3-address `dst = a <arith> b`, operands = slot or int immediate) and
`JumpUnlessIntCmp` (fused compare+branch). A `while` whose condition is an int
compare and whose body is int assignments (compound `s += i*i`, plain
`x = a*b+1`, `++`/`--`) compiles with **no tree-walker fallback**; **nested
expressions** use scratch temp slots (`compile_int_expr` + a temp register
allocator above the resolved locals; `Chunk::n_temps` grows the frame).
**Bool-safety:** a plain assign's rhs must be `definitely_int`
(arith/neg/int-literal, never a leaf id or comparison — both can be bool), since
writing an int into a bool slot would corrupt it. **Float** loops compile too
(`FloatBin`/`JumpUnlessFloatCmp`, operands promote), as do **mixed** int/float
loops (each condition/statement dispatched by its own kind) and **counted `for`
loops** — the last via a **fused `ForLoopStep`** superinstruction (`i += step` +
test + branch in one dispatch, matching the tree-walker's raw-C `ForRangeStmt`
counter; a naive 3-op encoding regressed +28%, the fused op wins). A counted
loop's **bound AND step may be non-trivial** — `for (i; i < len(s); i++)`,
`i < f()`, `i += st[0]` —
not just a slot/immediate: `try_native_for_range` compiles each once into
a reserved temp (the for-range specializer proved them loop-immutable) that
`ForLoopStep` re-reads each iteration, so a `len()`-bounded counted loop (and
nested forms) goes native instead of falling back — e.g. `30_str_index_iterate`
1.03x→0.80x vs the tree-walker. **The temps compile AFTER the loop's `init`**,
matching `ForRangeStmt::do_eval`'s init → bound → step order — compiling the
bound first evaluated it BEFORE a side-effecting init that changed it (a real
wrong-result `-vm` divergence, pinned by the "init evaluates BEFORE the
once-read bound" test). A general (non-range) `for` also lowers with **no
cond** (`for (;;)` — an unconditional loop exiting via break/return, like the
tree-walker) and with a **BOXED increment** (`out += "x"`, a dyn `d++` — the
same three-tier statement dispatch as any body statement). This is where
the VM *wins*: `01_while_loop` −50%, a nested int loop −61%, a pure-float loop
−71%, `03_int_arith` (top-level `for`) −72% instructions. **Phase 4** ran these
loops inside **function bodies** too (via `do_func_call`); since the NATIVE
CALL STACK landed (see its section below) a VM->VM call never goes through
`do_func_call` at all - the chunk is stamped on the function's
**`FuncDescriptor`** (stored in `g_func_chunks`, keyed by descriptor) and
the dispatch loop pushes/pops call records itself. **Compilation is AOT, not
lazy** (`vm_precompile_all`, run by `vm_compile`): it walks `collect_funcs`
(which recurses via the COMPLETE `for_each_child_of`, so a func declared in a
try body / slice / anywhere is not missed) and compiles EVERY function body
UPFRONT, stamping each descriptor's `vm_chunk` + `vm_chunk_tried`, so
`do_func_call` reads a precomputed pointer and never compiles at call time
(the maintainer's no-lazy rule + a `.myv`-serialization prerequisite; the lazy
`vm_func_chunk` miss path is a never-hit safety net). The per-function compile
+ gate is the shared **`codegen_func_body`** (codegen.cpp) — the single source
of truth for "which functions have bytecode", used by BOTH the precompile AND
the `-vd` dump, so `-vd` faithfully shows the real compiled chunk set. A
**dead base template is the ONLY exclusion** from that set: the inferencer
marks `FuncDescriptor::is_template_base` for a template NEVER used as a value
(`!value_used` — all its calls are direct and were redirected to `name$N`
instances, so the base never runs); a VALUE-used template (dict/var/arg-
dispatched INDIRECTLY, e.g. phonebook's `cmd_*`) is NOT marked, so its base
keeps its chunk (the indirect call runs the base body). So `55_float_sum` (a
loop in a function) is −62%. **EVERY other callable body keeps its chunk**
(post-teardown the chunk is the only way to run it): the old "has at least
one REAL op" gate is dropped (an empty body is a bare `Halt` returning none),
a ZERO-SLOT function (a param-less, local-less closure — `next_slot == 0`, so
the resolver leaves it `resolved == false`) runs its chunk through a
temps-only frame (`do_func_call`'s chunk hook is NOT gated on `resolved`),
and the one genuinely un-compilable body — the pathological un-slottable
>64-param function (its blocks are never `scope_free`) — is a loud
compile-time `NotLoweredEx`, never a silent tree-walk. A function chunk
(`codegen_chunk`) whose body ends in a `ReturnV` **and whose end nothing
BRANCHES TO** omits the trailing `Halt`: `ReturnV` already `return`s from
`vm_run_chunk`, so such a `Halt` is dead and unreferenced. **The second
half of that condition was missing until 2026-08-02** and the comment
asserted in prose that "the codegen emits no jump to the chunk end past a
return" — FALSE for a body whose last statement is a conditional return,
`func f(x) { ...; if (s > 0) return s; }`, where the `if`'s false arm (the
implicit `return none`) targets exactly the pc the `Halt` would occupy.
Dropping it left that branch one past the last instruction, so taking it
made `vm_dispatch` READ `code[n]`: an ASan-confirmed heap-buffer-overflow
yielding garbage (`-1`, or a STALE dst slot) instead of `none`, in the
DEFAULT build. Now checked (`visit_pc_fields` over the emitted code) rather
than asserted, with an ASSERTS-only invariant after the peephole that no pc
field points past the last instruction. Found by pushing coverage on the
bytecode splice — the shape reaches the splice's own tail-target rewrite.
A FALL-THROUGH body (`main`, a
void function, a trailing loop/if) keeps the `Halt` as its implicit-return-
`none` terminator + jump target. Recursion with arith stays ~neutral (`fib`
−0.08%).

**Array elements and nested control flow:** array element read/write
(`a[i]` / `a[i]=v` → `LoadElem`/`StoreElem`, mirroring the flat-array
subscript/store incl. COW; a dict subscript stays a fallback via the inferencer-
set `Subscript::base_array`), and **nested control flow** — the loop codegen
emits the body directly into the chunk with backpatching, so nested loops and an
`if` in a loop body go native (this is the broad win: many benchmarks wrap their
work loop in a `for(rep)` amplifier whose body — a loop — used to force the
whole thing to fall back). **Loop CONDITIONS are as capable as `if`
conditions:** `emit_cond_jumps` (the while/for helper) now falls through to the
boxed `JumpUnlessTrueV` path (the same one `if` uses) for a condition that isn't
a `TypedScalarExpr` — a **bool VARIABLE** (`while (flag)`), a bool var in a `&&`
conjunct (`while (flag && j < N)`, split per-conjunct so the bool one boxes and
the comparison stays a native `JumpUnlessIntCmp`), a `||` chain, or a dyn/string
truthiness test. Before, a bool-var loop cond bailed the WHOLE loop to an
`EvalStmt`; `try_native_for` was unified onto the same helper so a general
`for` gets it too. A **var-initialized loop** (`for (var k = i; ...)`) also goes
native: `emit_init` falls through to a boxed move (`compile_boxed_stmt`) before
`EvalStmt` — the raw int-store path rejects a bare-identifier rhs (it can't
prove int-not-bool for a raw store, since a bool is `th==i` too), but a boxed
move preserves the real type. A **15-level randomly-nested if/while/for
(optimized + general) spine lowers with ZERO `EvalStmt` fallback** (see the
`vm/codegen: deep 15-level nest` test + `bench/*/68_nested`, ~4x CPython; and
the `tests/nested_fuzz.py` differential fuzzer, which generates thousands of
random deep-nested programs + their Python twins and checks tree-walker == VM ==
CPython). The
register choice (over a stack machine, which the
already-M8-optimized tree-walker would beat) is also the right IR for the
eventual native x86-64 codegen. Full roadmap + phase order:
`plans/archived/bytecode-vm.md`.

**Build the VM FOUNDATIONS before optimizing a construct — do NOT bolt a native
op onto an incomplete architecture.** A native lowering that lacks the
architecture-level features it needs ends up SLOWER than the M8 tree-walker, not
faster — proven: the `foreach` nativization (a `ForeachBind` op) *regressed*
(19_foreach_indexed 0.27x→0.30x, geomean 4.02→4.01) and was reverted. Two root
causes, both foundational: **(1) it referenced the AST** (`Instr::node =
ForeachStmt*`), which is wrong on its own terms — a `node` pointer can't be
serialized (no `__pycache__`-style dump), can't be JIT'd to machine code, and is
8 dead bytes in a 64-byte `Instr` (2× an `EvalValue`; size + I-cache locality
matter here exactly as they do for `EvalValue`) — AND having the node made it
*easy* to reuse the tree-walker's boxing binding path instead of doing the work
in registers. **(2) Box/unbox + shuffled temporaries** (`arr_elem_boxed`
boxes a flat scalar, `bind_loop_var` builds a 48-byte `LValue`), which is the
SAME per-element cost the tree-walker already pays in `do_iter` — so the VM
only added dispatch overhead. **The VM NEVER needs to box/unbox.** A flat
int/float/bool loop var reads the raw scalar into its register slot; a
GENERAL loop var just binds the array element's *existing* `EvalValue` into its
slot (a copy, no box, no unbox) — which is not slower than the tree-walker, only
not as fast as the flat path. A construct only earns a native op once the
foundation exists to lower it to slot/constant-pool operands with no AST
reference and no round-trip boxing; **do NOT rush a construct-by-construct win —
the goal is outstanding end-state results, not an immediate delta.** The
foundations to build first: an **AST-free instruction** (op-data in a constant
pool as indices, loop vars as slot operands, locs in a `pc→loc` side table — so
`Instr` drops the `node` field and shrinks), and a **box-free value/slot flow**.
See `plans/archived/vm-fallback-elimination.md` (the foreach negative-result note) and
`[[vm-nativization-heuristic]]`.

**Foundation step 1 — the loc SIDE TABLE (`Chunk::locs`, done).** An op that can
throw records its caret `Loc` in `Chunk::locs` (a `{pc,start,end}` vector,
by pc, binary-searched by `Chunk::loc_at` on the throw path ONLY) instead of
carrying an `Instr::node` AST pointer just for the error loc. A post-codegen
pass, **`extract_locs` (codegen.cpp), runs on the FINISHED chunk** (so the
codegen's rollback is untouched) and, for the ops whose ONLY use of `node` was
the loc, records it and **NULLs `node`** — making them AST-free. It migrated the
div/mod carets of `IntBin`/`FloatBin` (throw via `vm_throw_div0(chunk, pc)`,
which uses `loc_at`, not `node`) and dropped the dead `node` from the
non-throwing `JumpUnlessIntCmp`/`JumpUnlessFloatCmp`/`ForLoopStep`. So the whole
register/loop CORE is now `node`-free. **The runtime `Instr` holds no AST
reference at all**: the codegen-transient handle lives on `CgInstr` and is
nulled when codegen finishes (`verify_ast_free`), so the bytecode has no AST
pointer in its instruction stream.

**Foundation step 2 — op-data into the CONST POOL (member key, started).**
`DictLoadInt`/`DictLoadFloat` (the typed `d.k` / `d[k]` read) are now fully
AST-free: a member's key is baked into `Chunk::consts` at codegen (`add_const`)
and `Instr::a` carries its index as an immediate (`a.is_lit` distinguishes a
member key from a subscript's key temp), so the handler needs no `MemberExpr::
memId`. The PRESENT-key path stays `dict_present_value` (hot); the MISSING-key
path (a default-dict insert or `KeyNotFoundEx`) now routes through the shared
`Type::subscript(for_write=false)` - the tree-walker's exact logic - with its
loc from the side table, NOT `node->eval_int/eval_float`. `extract_locs` records
the caret + nulls the node. So DictLoad joined the register core as node-free.
The same loc-side-table move then freed every op whose only remaining `node` use
was the error caret: **`SubscriptV`** (`base[idx]` via `Type::subscript`) and
the **boxed scalar ops `BinOpV`/`CompoundV`/`CmpV`** (their `num_bin_op` PMF is
baked from `aop`; the catch does `vm_stamp_loc(chunk, pc, e)`), plus **`LogV`**
(never throws - node was dead). The shared `vm_stamp_loc` helper (vm.cpp) does
the cold-path `chunk.loc_at(pc, ...)` stamp for them all. **`LoadGlobalV`** is
node-free too: its only use was the cold undefined-global error, whose NAME is
in `gfuncs`'s slot->name list and its loc in the side table. **`MemberV`**
(`base.member`) took the next op-data step - a **member-key pool** (`Chunk::
MemberKey` = the name as a dict key + the interned uid + the optional flag +
both carets member_read throws with), so the op is a bare pool index
(`Instr::a`); `member_read` was factored into `member_read_core(dval, memId,
memUid, optional, mstart, mend, bstart, bend)` shared by the tree-walker wrapper
and the VM. **`CallV`/`CachedCallV`** are node-free too: the callee name (an
undefined-slot error) is in `gfuncs`'s slot->name list, the caret in the loc
side table (recording the callee-IDENTIFIER loc, matching the tree-walker's
undefined-callee error), and the backtrace call-site loc - a PER-CALL value -
is NOT looked up per call: `do_func_call` gained `(const Chunk *call_ck, size_t
call_pc)` params and resolves `loc_at` in its EXISTING catch, on the error path
only, so the hot success path pays no lookup (`vm_call_func`/`vm_cached_call`
pass `&chunk, pc` not a `Loc`; fib stays 0.95x vs the tree-walker, and multi-
frame backtraces are byte-identical). **The foreach / array-read ops
(`LoadElem*`, `ArrLen`, `DictIter*`, `ForeachDyn*`, `UnpackElem*`,
`LoadStructField*`) are now node-free too** (the loc side table; `LoadElem*`'s
dead non-array `node->eval` else-branch - unreachable, `base_array` is proven -
became an `InternalErrorEx` net). The builtin-call ops
(`CallBuiltinV`/`LV`/`LVElem`) read the serializable `builtin_calls` pool and
`EmplaceStruct` the `emplace_sites` pool; `CallValueGenericV` reads the
`call_sites` pool. P8
exceptions are fully native (they no longer reach a fallback op). The FALLBACK-OP
AUDIT (`plans/archived/vm-fallback-elimination.md`) that followed found LIVE `EvalStmt`
fallbacks in several more shapes — **F-1..F-4, all now NATIVE**: the multi-assign
/ IdList forms (`MultiUnpackV`), the residual `foreach` shapes (2-var dyn
container, indexed general-value unpack), the discarded-result indirect call
(`CallValueV` as a statement), and the flat `array<PodStruct>` literal (the
fused `MakeStructArrayV`). **The last residual was the reflection builtins**,
resolved by what each needs (so the ONLY node-holder left is dev-only `show`):
- **`show()` — DEV-ONLY builtin, now COMPILE-TIME-FOLDED** (the
  `make_dev_builtin` category, `types.cpp`; `is_dev_builtin` /
  `g_dev_builtins_allowed`, `eval.h`): it decompiles the AST — and since it
  RETURNS a string, `fold_show_calls` (end of `specialize_types`, the final
  tree) replaces the whole call with that string as a `LiteralStr`: a
  directly-named top-level function renders via `render_func_code(decl)`, a
  non-identifier expression via `render_construct_code(arg)` (the arg is never
  evaluated, so folding moves no side effect — byte-identical to the runtime
  builtin's answer over the same final tree). Self-gating: the callee must be
  a resolved `SymKind::builtin` identifier, so the fold fires in
  scripts/harness only; the REPL (map-resident builtins) keeps the runtime
  builtin and its **`:show` meta-command is UNCHANGED**. `show` is still a
  **compile-time error in a script** (`reject_dev_builtins`, structural pass,
  `-nti` included; a user `func show` shadow is left alone). With the fold,
  show NEVER reaches codegen — which is what allowed deleting the last
  fallback op (see *THE NO-FAIL CODEGEN* below). The residue an identifier
  arg that is NOT a named top-level function (a var holding a func) stays a
  runtime call: fine under the tree-walker/REPL, a NotLoweredEx compile abort
  under -vm (harness-only territory).
- **`type`/`typestr`/`kindstr`/`decltype` — DONE (AST-free).** Two moves:
  (1) the inferencer's `fold_type_query` already bakes the answer into `args[0]`
  (a `LiteralStr`/`LiteralObj`) in the common case and sets **`CallExpr::tq_folded`**
  — so both engines **ELIDE** the folded call (return the baked `args[0]`): the
  VM at codegen (a plain `LoadConstV`/`LoadLiteralObjV`, no call), the
  tree-walker in `CallExpr`/`DirectBuiltinCallExpr::do_eval`. The flag (not a
  `dynamic_cast<Literal>` node check) is what makes it `-nti`-correct: under
  `-nti` no fold runs, so a user's `typestr("hi")` is NOT elided and must report
  `"str"`, not `"hi"`. (2) The rare NON-folded query (`-nti` / a still-`Unknown`
  arg type) is a **dual-ABI builtin** (`make_builtin_customv`): a custom `func`
  (tree-walker) and a `func_v` (the VM's `CallBuiltinV`), BOTH always building
  the `Type`/string from the runtime VALUE (`make_runtime_type_value` /
  `reflect_typeof` / `TypeNames`) — the folded literal never reaches them (it's
  elided), so there is no node-based folded/non-folded ambiguity and the two
  engines stay byte-identical. This also fixed a latent tree-walker `-nti` bug
  (the old `dynamic_cast<LiteralStr>` heuristic wrongly returned a user
  string-literal arg instead of its type). No new storage (struct defs are
  already in `Chunk::struct_defs` + each value's own `def`; the folded `Type`
  object is a serializable `LiteralObj`). So the reflection residual is now ONLY
  the dev-only `show` (above), which deliberately keeps its AST.

**Script-mode real-code fallbacks — a further sweep (all now NATIVE).** After
F-1..F-4, a `ML_DBG_FB` audit hook in codegen `emit()` (compiled out by default;
logs the rendered construct behind an `EvalStmt`)
found the remaining SCRIPT-mode shapes a real program can hit, each now native:
a **`foreach` over `array<bool>`** (`LoadElemBool`, a real-bool bind, above); a
**nested / boxed struct construction** (`StructCtorV`'s widened
`pod_ctor_arg_safe` gate + the new `StructCtorBoxedV`, above); a **bare
discarded-value expression statement** (`s[3];`, `a[i:j];`, `x + y;` —
`gen_stmt` compiles it into a scratch temp and drops the result, so an OOB /
missing-key / type error still throws with the byte-identical caret; a bare LEAF
identifier / scalar literal is skipped, since the tree-walker never RValue-s a
discarded statement, so an undefined name stays its harmless `UndefinedId` no-op
rather than a `LoadGlobalV` throw); and an **inc-dec used as a VALUE**
(`y = x++`, `y = a[i]++` — `compile_boxed_expr`'s `IncDecExpr` case lowers it to
read-lvalue + mutate (postfix) / mutate + read (prefix), reusing the statement
compilers for the in-place mutation, gated by `incdec_lvalue_pure` on a side-
effect-free lvalue so the two evals of the slot/index agree). This last one
surfaced a **pre-existing tree-walker bug**: auto-const promoting a write-once
INDEX var (`var i=1; a[i]++`) dropped its decl but `fold_reads` had no
`IncDecExpr` case, dangling the promoted `i` — fixed by folding the READS in an
inc-dec lvalue like an assignment lvalue. **Net: ALL of `bench/` and `samples/`
lower 100% native and fully serializable — the struct benches' last
`EmplaceStruct` ctor node went too (its def + carets moved into the
serializable `emplace_sites` pool).**

**Error-path constructs → native throwing ops (`ThrowRuntimeV`).** The
always-throwing constructs the tree-walker ran and threw on are now native: an
UNRESOLVED name in an rvalue/callee position (`var y = foobar` /
`undefined_fn()` / `undef(5)` / a `_` read), an assignment to a scalar LITERAL
(`0 = 99`, `true = false`, a const-inlined `K = 6`) or a BUILTIN (`print = 5`),
and a REQUIRES-lvalue builtin (`append`/`push`/`pop`/`insert`/`erase`/`intptr`)
on a provably-non-lvalue arg0 (`append([1,2], 3)` — the only lvalues are
id/subscript/member), and a **REBIND of a runtime `const`** (`const c = [..]; c
= x` / `c += x`, a func/array/dict kept in a slot → `CannotRebindConstEx`; a
const *scalar* is inlined, so its rebind hits the bad-lvalue throw instead). New
op **`ThrowRuntimeV`** + a serializable
`Chunk::throws` pool (`{ThrowKind, Loc, name}`) throws the pooled exception with
the exact caret
— byte-identical, AST-free. Codegen: an `SymKind::unresolved` id in
`compile_boxed_expr` (a CALL with an unresolved callee throws before its args,
matching `what->eval` first); a bad-lvalue in `compile_boxed_stmt` (rhs compiled
FIRST for its side effects, then the throw, matching the tree-walker's rhs-then-
target order); the same rhs-first + throw for a `const` rebind (the rhs's side
effects run before `CannotRebindConstEx`, for both a plain and a compound
rebind); a non-lvalue arg0 in `try_native_mutating_builtin` (gated by
`builtin_requires_lvalue_arg0`, which EXCLUDES `sort`/`rev_sort`/`reverse` —
they accept a value arg0 and sort the copy). The bare-LEAF guard keeps a
discarded `foobar;` a no-op.

**A DYN callee → a generic value-call (`CallValueGenericV`).** An indirect call
of a `dyn` callee (`var dyn a = len; a("hi")`, `a(1)` on a non-func) is native.
A dyn callee is resolved at RUNTIME and may be a `FuncObject`, a read-only
`Builtin`, a MUTATING builtin (needs an lvalue arg), an AST builtin (`defined` —
needs the unevaluated arg node), or a struct descriptor, so the dispatch is
AST-dependent TODAY — the op
KEEPS its CallExpr node (its args ExprList + callee caret), but the callee LOAD
is native and a `FuncObject` body runs native (the do_func_call hook). The
dispatch is the shared **`dispatch_call_value`** helper (`eval.cpp`) reused by
BOTH the tree-walker's `CallExpr::do_eval` AND the op, so the two engines are
byte-identical (FuncObject → do_func_call, Builtin → its ExprList ABI, struct →
construct, else `NotCallableEx`). `extract_locs` records the op's CALL-SITE loc
(for a FuncObject body's backtrace) WHILE keeping the node — the LAST
non-fallback node-keeping op. A Func-TYPED callee still uses the leaner
register-run `CallValueV`. **The LAZY-ARG rule (fork F1 step 1, LANDED
2026-07-14):** a LAZY-ARG builtin — **`defined`/`isconst`/`isconstdecl`**,
whose argument is a NODE property that is never evaluated (`decltype` turned
out NOT lazy: like `type`/`typestr`/`kindstr` it is dual-ABI, its `func_v`
builds from the runtime value, so the type queries stay usable as values;
`show` was already script-rejected) — **cannot be used as a VALUE in a
script** (only called directly): `var dyn f = defined;`, `[isconst]`, or
passing one as an argument is a compile-time `SyntaxErrorEx`. Implemented as
a COMPILE-TIME reject only (`is_lazy_builtin`/`mark_lazy_builtin`,
types.cpp; the check rides the `reject_dev_builtins` walk, same
`g_dev_builtins_allowed` gate — the REPL retains the AST, so the indirect
form keeps working there, the `show` precedent). A runtime check in
`dispatch_call_value` is unnecessary: a script can no longer produce such a
value. Documented in README (`defined`, `isconst`). **Step 2 (LANDED
2026-07-14) — the op is AST-FREE.** Two pieces:
(1) **Every `Builtin` carries a `Kind` tag** (value / lvalue / map / filter /
lazy / node — `evalvalue.h`; Builtin grows 16→24 bytes, still inside the
24-byte `EvalValue` payload), so an indirect callee's ABI is decidable from
the VALUE at runtime (a direct call knows it at compile time; an indirect
one cannot). Every `make_*` registration stamps it.
(2) **Args pre-evaluated + the `Chunk::CallSite` pool** (`b` = nargs |
site << 12): the ArgLocs data (whole-args + per-arg carets, arr_hint) PLUS
**arg0's LVALUE DESCRIPTOR** — the by-ref encoding. Frame slots can't hold
an `LValue*`-boxed value (`LValue::type_checks`), so instead of
materializing the tree-walker's raw arg value the dispatch RE-DERIVES
arg0's `LValue*` from the descriptor, only when the callee turns out
`func_lv` (the `CallBuiltinLV`/`LVElem`/`LVMember` model): forms none
(a non-lvalue expr → null target → `NotLValueEx`), slot (id kind+slot; an
undefined GLOBAL surfaces at the CONSUMER as the tree-walker's raw
`UndefinedId` does), elem (base kind/slot + a RESERVED index temp; run[0]
holds the element VALUE, filled by an ordinary `SubscriptV` at arg0's
position so OOB/key throws keep argument order; the func_lv re-derive
repeats the subscript — idempotent, and a between-args container mutation
THROWS where the tree-walker's stale `LValue*` is UB — safer), member
(member_keys idx; run[0] via `MemberV`), undef (a pooled name). So the
SLICE WRITE-BACK, const, and literal-arg0 errors of an indirect
`append`/`sort` are byte-identical to the direct call. A **`CheckCallableV`**
op sits between the callee and the arg run (a non-callable callee throws
`NotCallableEx` BEFORE the args evaluate, the tree-walker's order — the
earlier FuncObject-only version was reverted for rejecting builtin callees;
this one admits all three callable kinds). Dispatch: FuncObject →
`vm_call_func` (run[0] filled with the derived RValue — an undefined name
throws at BIND); struct → **`construct_struct_v`** (the values twin of
construct_struct: full runtime arity + per-field coerce with pooled
carets); Builtin → **`dispatch_builtin_values`** by Kind. **The EAGER-ARGS
language rule:** an INDIRECT builtin call evaluates its args first, then
the builtin's checks run — implemented in the ONE shared
`dispatch_builtin_values` (eval.cpp) that BOTH engines call (the
tree-walker's `dispatch_call_value` routes a `vm_dyn_callee`-stamped call
with a non-lazy Builtin through raw left-to-right arg evals + the same
dispatch; a DIRECT tree-walked call — const-eval, REPL, unspecialized —
keeps the node ABI, so `map`'s validate-before-arg1 and `sort`'s custom
arg0 are unchanged there). README documents the rule under `map`. Known
corner (documented, not observable in well-formed code): an UNRESOLVED id
in a non-arg0 position throws at its own position under the VM
(`ThrowRuntimeV` in the run) but at the consumer in the tree-walker, so a
LATER arg's side effects may run in one engine and not the other — both
throw the same `UndefinedVariableEx`.

**Niche STATEMENTS → native (a further sweep).** Several residual real shapes
went native: an **inc-dec STATEMENT on an int/float member/nested subscript**
(`p.x++`, `a[i][j]++` == the compound store `lvalue += 1`, via
compile_int_stmt's IncDecExpr handler → StoreMemberV / DictStore / StoreElem2V;
gated on a PROVEN int/float lvalue — inc-dec is int/float-only, so a dyn field
falls back), the **VALUE form** of those (`o = p.x++` — `incdec_lvalue_pure`
widened to a member/nested lvalue), a **whole-`p` flat-struct-array `foreach`**
(`foreach (var p in a)` using `p` as a value — **`LoadStructElemV`**
materializes a fresh StructObject per iteration, byte-identical to the
tree-walker's reused-object bind, so `append(o, p)` / `q = p` go native;
scalar-field-only bodies keep the faster direct read), and a **compound
multi-target assign** (`a, b += rhs` — `MultiUnpackV` gained a base op: each
target reads its current value, applies the op with its element/scalar, writes
back). A **dyn SCALAR** inc-dec (`d++` on a dyn/general local/global/capture) is
native via **`IncDecCheckedV`** (reads the value, THROWS `TypeErrorEx` unless
int/float — inc-dec is int/float-ONLY, unlike a compound `+= 1` which would
concat a string — then ±1), and a **dyn ELEMENT** inc-dec `c[k]++`/`c[k]--` (a
dyn dict / general array / dyn base — anything NOT a proven flat int/float
element, which `compile_int/float_stmt` already handle) via
**`IncDecElemCheckedV`**: it forms the element LValue via the runtime
`subscript(for_write=false)` and does the same int/float-checked ±1, mirroring
`IncDecExpr::do_eval`'s dyn read-modify-write (a flat scalar element has no
LValue → `NotLValueEx`, exactly as the tree-walker). It is **AST-FREE**: its
TWO distinct error carets — the SUBSCRIPT loc for a subscript-internal throw
(`KeyNotFound`/OOB) vs the INC-DEC loc for its own `NotLValue`/`const`/
`TypeError` — which a one-loc-per-pc side table can't hold, live in the
serializable **`Chunk::incdec_sites`** pool (`Instr::b` = the index, O(1));
the undefined-global-BASE caret comes from the **`base_locs`** side table
(`vm_store_base`, node = null; #127 — see below).
A **dyn MEMBER** inc-dec `d.f++`/`d.f--` (a
dyn/general base holding a struct or dict) is the exact twin,
**`IncDecMemberCheckedV`**:
it forms the member LValue like `MemberExpr::do_eval`'s rooted-base path (a
mutable boxed STRUCT field or a DICT value is an lvalue; a POD field / readonly
/ a missing dict key throws `NotLValueEx`/`KeyNotFoundEx`), int/float-checks,
±1. Same pool-carried dual-loc (the MEMBER loc for a `KeyNotFound` vs the
INC-DEC loc for `NotLValue`/`TypeError`), plus the member key
(`memId`/`memUid` ride in the same `incdec_sites` entry). Both cover a
proven-struct NON-numeric member
too (`s.name++` on a str field → `TypeError`, `th != i/f` so it isn't the M8
`StoreMemberV` path). An optional `d?.f++` still falls back (rare).

**Non-tail block-body inline (`InlinedCallExpr`) → native.** A `y = f(args)`
whose block-bodied `f` inlined with a residual that couldn't collapse to a
ternary (a leading side-effecting statement, a reassigned local) is an
`InlinedCall(Block(...))` — the callee's body run behind its OWN return
boundary (a `return v` inside yields THIS expr's value, not the enclosing
function's; `InlinedCallExpr::do_eval` swaps `ctx->flow`). The VM lowers it via
a **scoped return boundary** in the codegen (an `inline_returns` stack, like the
loop stack): `compile_boxed_expr` inits a result slot to `none` (a fall-through
body yields none), compiles the body's statements inline, and `try_native_
return` — gated on `inline_returns` — redirects each `return` to "**MoveV into
the result slot + Jump to the body end**" instead of `ReturnV`-ing the whole
chunk. A body statement/return that can't lower (a try crossed INSIDE the
boundary) fails the whole inline → the tree-walker runs the `InlinedCallExpr`
(byte-identical). This surfaced + FIXED a latent bug: `make_typed`
(`specialize_types`) hand-copied base fields and **dropped `inline_ctx`**, so a
specialized arith node inside an inlined body lost its inlined-at chain — the
tree-walker hid it (the Block wrapper flushes) but the VM's flat body has no
wrapper, so a backtrace crossing it dropped the virtual frame; `make_typed` now
uses `copy_base_fields`. So an error inside a native InlinedCall shows the
byte-identical virtual `f$0` frame.

**Typed NON-SCALAR decls/assigns → native (a plain boxed store).** An
explicitly-typed decl/reassign/compound whose declared type is `bool` / `str` /
`array<…>` / `dict<…>` / a struct (`str s = ..`, `array<int> a = ..`,
`Point p = P(..)`, `s += ".."`, and their global/capture/const forms) is now a
plain `StoreGlobalV`/`StoreCaptureV`/`DeclConstV`/retarget store, NOT an
`EvalStmt`. Sound because **`coerce_to_decl_type` is a no-op for every declared
type EXCEPT `int`/`float`** (`eval.cpp` — it returns the value unchanged; the
type is proven at compile time by the inferencer), and the tree-walker's
decl path only calls the coerce for `DeclType::i`/`f` (`handle_single_expr14`).
The `i`/`f` decline is gone too: a typed int/float PLAIN assign fed a boxed
value now lowers to **`CoerceNumV`** (dst = `coerce_to_decl_type(src)` — the
widen / pass-none / dyn-narrowing-throw, the exact function the tree-walker
runs; for a LOCAL lvalue the op IS the store, a global/capture store coerces
into a temp first; the Expr14-span caret rides the loc side table). Its live
producer is the inferencer's `coerces_dyn` accumulator stamp — `var s = 0;
s = s + d` was the last `EvalStmt` in a plain accumulator body (an EXPLICIT
`int x = <dyn>` is compile-rejected, so it never reaches codegen). A COMPOUND
(`s += d`) does NOT coerce (`handle_single_expr14` coerces `op == assign`
only) and lowers as a plain `CompoundV`/`StoreGlobalV` compound.
A typed array/dict keeps its FLAT
storage (the `ArrHint` rides on the rvalue node, honored by `MakeArrayV`/
`LoadLiteralObjV` regardless of the decl), and an uninitialized `array<int> a;`
/ `str s;` / `Point p;` is already desugared (at parse) to a zero-value literal
/ zero-struct ctor rvalue, so it lowers the same way.

**`EvalToSlot` + `JumpIfFalse` DELETED (2026-07-14)** — the step before the
no-fail codegen below, which took `EvalStmt` too. The last live expression/
statement roots went native first: **coalesce** `a ?? b` (a
`compile_boxed_expr` case: MoveV lhs into the dst — reserved BELOW the
scratch temps — + the new **`JumpIfNotNoneV`** op to skip the rhs, so the
`??` short-circuit is preserved); **chained boxed comparisons**
`x == y == z` (the `emit_boxed_chain` 2-operand limit removed — the chain
loop already accumulates left-to-right like the tree-walker);
**assignment as an EXPRESSION** `a = (b = x + 1) + 2` (an `Expr14` case in
`compile_boxed_expr` for a resolved-local non-const id target: run the
int/float/boxed STATEMENT compilers, then use the target's slot as the
operand — this exposed and fixed a retarget-guard bug where the
plain-assign retarget could steal an inner store's dst: it now requires
`rslot >= temp_base`); and the **typed IdList destructure** `fa, fb =
[1, 2]` with int/float-annotated targets (`try_multi_unpack` accepts
them; a per-target coerce vector rides the serializable
**`Chunk::unpack_coerce`** pool — parallel to `unpack_targets`,
`Instr::b` indexes it — and MultiUnpackV runs `vm_coerce_decl_num` per
store, same TypeErrorEx + caret as the tree-walker). With those native,
the two per-fragment fallback ops became unreachable and were REMOVED
(opcodes, handlers, `vm_eval_cond`, `eval_to_temp`, the Phase-1
`gen_if`/`gen_while` flatten forms, disasm renders). Decline semantics
now: an operand/condition/loop-init that can't lower fails its WHOLE
statement to one `EvalStmt` (`emit_init` returns bool; `compile_native_if`
failure → a whole-if `EvalStmt` in `gen_stmt`) — never a per-op node
fallback. **R4 is now NATIVE too — `IncDecChainV`** (the impure-lvalue
inc-dec VALUE form, `y = a[f()]++` / `++d[kf()]` / `a[f()][g()]++` /
`a[f()].x++` / `mk()[0]++`): the codegen decomposes the lvalue into a
root (a container slot, or a compiled RVALUE temp — kind 3, whose VALUE
seed keeps the tree-walker's rvalue-ness, so `mk()[0]++` still throws
NotLValueEx) plus member/subscript steps with each KEY compiled into a
temp ONCE (side effects run exactly once, in source order), pooled in
the serializable **`Chunk::incdec_chains`** (steps + tier + prefix +
the `allow_flat`/`allow_pod` gates — `no_side_effects(final base)`,
the tree-walker's own AST-shape-dependent try_flat/try_pod gate, so it
is compile-time data — + carets). The runtime walk is the shared
StoreLValueChainV intermediate walk (`vm_chain_walk`); the final step
runs `vm_incdec_final` (eval.cpp) — IncDecExpr::do_eval's EXACT tier
semantics: tier 2 (proven int/float) = the compound `±= 1`
(flat_store_core / member-store / general-lvalue slot_rmw) then
old = new ∓ 1 derived with NO re-read; tier 3 (dyn) = the checked
read-modify-write. A follow-up closed the LAST real-code emitter: a
**nested named func/struct decl inside a loop/if body** (a scoped
global) now lowers via the shared `emit_func_decl`/`emit_struct_decl`
(MakeClosureV/LoadConstV + StoreGlobalV — re-bound each iteration,
exactly as the tree-walker re-evals the decl; gen_stmt already covered
top-level/function-body decls, the loop/if body compiler did not).
**THE NO-FAIL CODEGEN (2026-07-15, maintainer-directed): `EvalStmt` is
DELETED — there is NO fallback op AT ALL.** Two moves made the codegen
total: (1) **`show()` folds at COMPILE time** (`fold_show_calls`, end of
`specialize_types` — the tree is final there, so the fold renders exactly
what the runtime builtin rendered; show already RETURNED a string, so the
call becomes a `LiteralStr`): a directly-named top-level function →
`render_func_code(decl)`, a non-identifier expression →
`render_construct_code(arg)`; self-gating on a `SymKind::builtin` callee,
so it fires in scripts/harness only — the REPL (map-resident builtins)
keeps the runtime builtin and **`:show` is untouched** (a meta-command,
never codegen). A user `func show` shadow is `SymKind::global` → never
folded. (2) The former decline NETS are **`throw_not_lowered` /
`NotLoweredEx`** — an always-on (release included) compile ABORT naming
the construct: every statement/expression either lowers or the compiler
REFUSES loudly; a future gap cannot hide as a silent tree-walk. Flipping
the nets flushed out + fixed the last REAL gaps, each now native: a
typed `!x` as a VALUE (`Cat::lnot` → boxed UnaryV — undetected while
expr bodies had no chunks), EMPTY loop/foreach bodies (the obsolete
"all-fallback body" gate dropped), the loop-body DISCARD tier (any
expression statement — a discarded `map(...)`, a ctor — compiles into a
scratch temp; inert leaves skip, an unresolved/global bare id keeps its
throw-or-noop semantics), an unresolved-lvalue assignment in a function
(rhs side effects, then the pooled UndefinedVariableEx), zero-arg lvalue
builtins (`intptr()` → pooled InvalidNumberOfArgsEx), the sort family
with a VALUE arg0 (`sort(clone(a))` — materialized into a temp slot),
the CHECKED POD ctor (`Point(dynval, 2)` — routed through
StructCtorBoxedV's pooled per-arg carets; `construct_struct_boxed_from_
values` dispatches on `is_pod`, and a non-qualifying `append(arr,
P(dyn))` compiles the ctor as a rest VALUE), a dyn-base member store
(`p.x = 5` in a dyn-param body — a single-MEMBER StoreLValueChainV;
proven bases keep StoreMemberV/DictStore priority), and **foreach is
UNIVERSAL** (the ForeachDyn ops lost their `container_is_dyn` gate —
they runtime-dispatch array|dict exactly like `do_iter`, so they are the
catch-all tried after the fast forms: a lone `_`, a 1-var indexed array,
a >2-var none-padded dict all lower). **Deleted with the op:**
`Chunk::node_table` + `node_at_pc`, the `Chunk::ast_nodes` pool (now a
CODEGEN-object scratch; `build_node_table` became `verify_ast_free` —
every `node_idx` must be nulled when codegen finishes), the disasm
`node_table` section (the "NOT serializable" signal is now a compile
refusal instead), and the `ML_DBG_FB` hook. `Chunk`'s only remaining
`Construct*`-typed member is GONE too since the FuncDescriptor step
(`closure_defs` holds descriptors).
The `InlinedCallExpr` return-across-try residue is CLOSED: a `return`
crossing trys INSIDE the inline boundary inlines each crossed try's
handler-pop + finally at the return (bounded at the BOUNDARY, not the
function — the same `inline_crossed_finallys` chaining a real return uses,
with the value copied to a protected temp before the finallys), then yields
via the boundary's MoveV+Jump. Flow across NESTED trys (any depth) was
already fully chained by `inline_crossed_finallys` — pinned by a
2-finally break/return test.
None appear in `bench/` or `samples/` (both stay 100% native). The `_`-in-unpack
/ indexed dict `foreach`, the ≥3-level SUBSCRIPT nested store, the **general
member/subscript lvalue-chain store** (`a[i].f=v` / `q.p.x=v` / `d.a[0].f=v`),
the dyn scalar/element/member inc-dec, `append` to a struct member, the common
`InlinedCallExpr`, typed NON-scalar decls, EVERY `defined` form (fold /
`DefinedGlobalV` / the non-identifier arg / wrong arity), and a
runtime-`const` rebind are all now native (above).

**WHERE AN OP'S DATA COMES FROM (all of it serializable).** The builtin-call
ops read the `builtin_calls` pool, `EmplaceStruct` the `emplace_sites` pool
(ctor def + container/field carets), `CallValueGenericV` the `call_sites` pool
(carets + arg0's lvalue descriptor), `CheckFuncV`/`MapFilterV` and the flat
int/float element stores the loc side table, and the checked inc-decs the
`incdec_sites` / `incdec_chains` pools.

**`Chunk::base_locs` — the container store's SECOND caret (#127).** Identical
shape to `locs` (pc-keyed, ascending, binary-searched, read only on the throw
path) and SPARSE: only a store whose base can be a **global** records one. It
exists because a store has two error spans and one `locs` entry cannot hold
both — an OOB / key / type error carets the WHOLE lvalue (`g[0]`, which is
what `locs` holds and what the tree-walker marks), while an **unbound global
base** carets only the base identifier (`g`), which is what
`Identifier::do_eval` throws with in the tree-walker. Every store form was
reporting the whole subscript, and the CHAIN stores — whose per-step carets
live in `chain_locs`, so they record no `locs` entry at all — reported **no
location whatsoever** (a backtrace rendering "line 0"). Produced by
`extract_locs` from `CgInstr::base_node_idx`, which the emit sites set from
`as_container_base` via `add_base_node` (a non-global base records nothing).
`vm_store_base` prefers it and falls back to `locs`. It rides the two JIT pc
remaps and the bytecode splice's rebuild like `locs`, and is serialized right
after it (myv **v13**).
The AST-node side table this section used to describe -
`Chunk::node_table`/`node_at_pc` and the `ast_nodes` pool - is GONE with the
fallback op it existed for; the codegen-transient handle now lives on
`CgInstr`, not `Instr` (B3 stage 2, docs/jit-optimizations.md), and
`verify_ast_free` asserts every
one is nulled when codegen finishes.

**A THIRD side table — `Chunk::inline_ctxs` (`pc → inline_frames index`, P8 Inc
4).** Same shape/cost as `locs` (sorted, binary-searched, throw path only),
populated by `extract_locs` from an op's `node->inline_ctx`: it records the
"inlined-at" chain of any op spliced from an inlined body, so a backtrace
crossing inlined code shows the virtual frames under the VM too
(`vm_flush_inline`, flushed at `vm_raise` / a call-op signal-propagation / the
boundary catch). **It is now SERIALIZABLE (no AST pointer):** the `InlineCtx*`
chain is FLATTENED into the **`Chunk::inline_frames`** pool — each entry pure
data (`callee_name` + `params` + `call_site` loc + a **parent index** into the
same pool, `-1` == root; a parent always has a lower index) — and the side table
maps `pc → a pool index`. `intern_inline_ctx` (codegen) flattens each op's chain
in `extract_locs`, deduping shared chains via a memo (interning the parent
first, so the pool is topologically ordered); `vm_flush_inline` walks the pool
by parent index, pushing the SAME `BacktraceFrame`s a tree-walker
`flush_inline_frames` over the `InlineCtx` chain would (byte-identical, single
AND nested/recursion-unroll inlines). This is the analogue of the `chain_locs`
per-step-loc pool — the last non-`locs` side table off the AST. (The tree-walker
still uses the `InlineCtx*`-based `flush_inline_frames` directly, from
`node->inline_ctx`.)

**The disassembler dumps the WHOLE serializable image, not just funcs
(`disasm.cpp`, `-vd`).** `disassemble_program` prints the program's custom TYPES
(every `struct` def - name, POD byte-offset / boxed-slot layout, folded consts)
first, then each chunk's code, then that chunk's POOLS + side tables (`consts`,
`member_keys`, `boxed_ops` (labelled *derived* - the model-flip nativize-ops
JIT-bakeable operand data for BinOpV/CmpV/CompoundV; rebuilt on a `.myv` load,
not primary), `incdec_sites`, `incdec_chains`, `emplace_sites`,
`call_sites`, `catch_types`,
`literal_objs`, `closure_defs`, `struct_defs`,
`unpack_targets`, `chain_locs`, `locs`, `inline_ctxs` + its `inline_frames`
pool - non-empty ones only). This is the audit surface for the `.myv` stored
bytecode: everything a serialized file must hold is visible in the dump, and
every section in it IS serializable - the no-fail codegen and the flattened
`inline_frames` pool between them removed the last one that was not. Each
chunk's header also shows its **CONTAINER PLAN**
(model-flip M1, `jit_container_plan`, `jit.cpp`): how the body partitions into
NATIVE vs ISLAND segments and whether it could be ONE native container —
`READY` (every op native-ELIGIBLE, a looser bar than `native_leaf`: an op can
be run-eligible yet sit in a run whose compilation declined) or `NOT ready`
with each blocking
island's pc span + its distinct un-nativizable opcodes (the "what to nativize
next" surface). DUMP-ONLY today; see **THE MODEL FLIP** below and
`plans/archived/model-flip.md`.
