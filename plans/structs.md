# Custom struct types — implementation plan

> **BUILD STATUS (live).** **ALL 8 phases are DONE and committed.** The full
> boxed feature (decl, construction, field/`const` access, inference,
> const-fold, COW) **plus** phase 5 (POD C-layout), phase 6 (nested/recursive
> POD — with a `check_struct_no_recursion` parser pass that rejects an
> infinitely-recursive non-opt struct field, pointing the user at a nullable
> `dyn? <field>` back-edge), phase 7 (flat `array<PodStruct>` storage with a
> COW-reuse `foreach` and
> an auto-promote cold path), and **phase 8** — M8-style *direct* unboxed field
> access (`MemberExpr::eval_int`/`eval_float`; `a[i].field` reads straight from
> the array bytes via `member_pod_array_scalar`, no materialization; the
> compound-assign fast path treats a `th==i` rhs as an `eval_int` read) **and**
> construct-in-place append (`append(arr, S(..))` builds into the array buffer
> via `try_construct_into_struct_array`, no temporary `StructObject`). 885 `-rt`
> tests pass (ASan), coverage-hardened; README + CLAUDE document it.
> **Perf:** `a[i].x` field access is within ~6% of `array<int>`'s `a[i]`;
> construction stays object-bound but the per-element alloc is gone (build
> overhead 2.47x→1.76x vs `array<int>`; `bench/58_structs` ~26x→~21x CPython).
> The validated patterns held: a POD field *write* reuses the
> `try_flat_subscript_store` pattern (a direct typed store bypassing the lvalue
> model), and member access must respect `is_lvalue_rooted` (a field lvalue is
> handed out only for a variable-rooted base, never a temporary — the UAF fix in
> eval.cpp). Remaining (beyond v1 scope): `var` fields, `opt` scalar fields,
> methods, empty structs, and a foreach-body specialization that would avoid the
> loop-var `StructObject` entirely.

Status: **designed, ready to build.** The prerequisites the original version of
this plan was waiting on have all landed, which changes the design materially:

- **Static type inference** (`plans/type-inference.md`) is in. `STyKind::Struct`
  + the `struct_def` slot are already reserved in `stype.h`. Field access can be
  a compile-time slot/offset, not a runtime table lookup.
- **Named arguments** are in: `ExprList::arg_names` plus the shared
  `desugar_named_call(call, vector<ParamSpec>)` in `syntax.cpp`. A struct
  constructor is *just another callee with a parameter list*, so construction
  reuses this verbatim — positional, named, and mixed, with provably zero perf
  cost (it desugars to positional before any optimizer runs).
- **Flat (unboxed) typed arrays** are in: `SharedObject`'s `Storage` union
  (`general`/`ints`/`floats`/`bools`) + the `ArrHint` mechanism. This is the
  template for `array<PodStruct>` (a new flat storage kind).

This rewrite folds in the user's new requirements: struct init reuses the call
syntax (positional **and** named, no perf impact); fields take the same explicit
types as variables (`bool/int/float/array/dict` + struct types + `dyn`); an
all-scalar struct gets a **native-C memory layout** and `array<ThatStruct>` is
stored **flat/unboxed** like `array<int>`; `var` fields and `opt` *scalar*
fields are deferred; type inference *about* a struct type (outside it) always
works; `const` works with no limitations.

---

## 1. Goal & surface syntax

C-like user-defined value types: named, explicitly-typed fields, plus
type-level `const`s. No methods in v1.

```
struct Point {
    int x;                 # explicit type, like a variable decl
    int y;
    const ORIGIN = "o";    # a type-level const (folded once)
}

var p = Point(1, 2);          # positional construction
var q = Point(x: 3, y: 4);    # named (reuses named-arg machinery)
var r = Point(1, y: 2);       # mixed: positional then named (same rules)
p.x;                          # field read   -> 1
Point.ORIGIN;                 # const via type     -> "o"
p.ORIGIN;                     # const via instance -> "o"

var line = [Point(0,0), Point(1,1)];   # inferred array<Point>, stored FLAT
```

Construction *looks* identical to a function call, and that is deliberate: the
grammar stays context-free because `Point(...)` **is parsed as a `CallExpr`**.
What makes it a construction rather than a call is only the *type* of the
callee — a struct type descriptor instead of a function — decided in the
inferencer and at eval time, never in the grammar.

---

## 2. The central model: a struct is a callee with a parameter list

Treat each `struct` as defining a **compiler-generated constructor** whose
parameter list is the field list, in declaration order. Everything else falls
out of machinery that already exists:

- **Parsing:** `Point(a, x: b)` is an ordinary `CallExpr` carrying
  `ExprList::arg_names`. No new call syntax.
- **Named/mixed/positional + const-fold:** the parser's `pTryDesugarNamedCall`
  and the inferencer's `lower_named_args` already desugar a named call to
  positional via `desugar_named_call(call, vector<ParamSpec>)`. Extend *only the
  callee-resolution step* in each to recognize a struct-type callee and build
  the `ParamSpec` view from the struct's fields (`{field_name, is_opt}`). The
  ordering rules, the `none`-fill of skipped opts, the duplicate / unknown /
  missing-required errors, and the "same optimization as positional" guarantee
  are then **inherited for free**.
- **Arity/type checking:** the inferencer's `check_call` treats the field list
  as the param list — a wrong-typed or missing field is the same machinery as a
  wrong-typed/missing argument.
- **Construction:** `CallExpr::do_eval` gains one branch — callee is a struct
  type descriptor → build a `StructObject`.

This is why the old "no mixing named & positional" decision is **dropped**: a
constructor is a call, so it follows the *call* rule (a leading run of
positional args, then named args in field-declaration order; reordering is an
error). One rule, one implementation.

---

## 3. Type model

### Static (inferencer, `stype.h` — slot already reserved)

`STyKind::Struct` with `struct_def` pointing at the `StructTypeDef`. Lattice ops
(`stype.cpp`):

- `equal(a,b)` for two Structs: same `struct_def` (nominal, no structural
  equivalence).
- `assignable(src,dst)`: equal struct types only (no subtyping in v1); plus the
  usual `none -> opt`, `T -> dyn`.
- `join`: same struct → that struct; different structs (or struct vs other) →
  `dyn` (so `[Point(..), Circle(..)]` is `array<dyn>`, heterogeneous).
- `to_string`: the struct's name (`"Point"`, `"opt Point"`).

A constructor call `Point(...)` types to `Struct(Point)`. `array<Point>` is a
normal `Array` whose `elem` is `Struct(Point)` — so `var a = [Point(1,2)]`
infers `array<Point>` with **no new inference rules**, satisfying "inference
*about* the struct type always works".

### Runtime (`type.h`, `evalvalue.h`, `types.cpp`)

Two new `TypeE`s, mirroring func's decl/object split:

1. **`t_structtype`** — the **type descriptor**. Holds a raw `StructTypeDef *`
   (program-lifetime, AST-owned, exactly like `FuncObject::func`). Trivial
   (placed `< t_str`). It is the value the `struct` decl binds in scope; it is
   `const`, supports `.CONST` reads, and is *callable* to construct.
2. **`t_struct`** — an **instance**. Holds `intrusive_ptr<StructObject>`
   (non-trivial, appended after `t_dict`, `>= t_str`). COW value semantics.

Wire both through `TypeE`, `TypeToEnum`, `ValueU` (a trivial `StructTypeDef*`
member and a `FlatVal<intrusive_ptr<StructObject>>` member), `TypeNames`,
`AllTypes`, and new `TypeStructType` / `TypeStruct` classes in
`src/types/struct.cpp.h` (`#include`d into `types.cpp`).

---

## 4. Field types, restrictions, and the layout split

A field declaration is `TYPE name;` using the **same explicit types as
variables** (`DeclType` for `bool/int/float/array/dict`) plus **struct-type
names** and **`dyn`**. v1 restrictions (each one is what keeps the design
tractable; all are liftable later):

- **No `var` fields** (deferred — see §11). Every field needs an explicit type,
  so field types are fully known *after parsing*, with no whole-program
  fixpoint. A `var`/inferred field is a compile error pointing the user at an
  explicit type.
- **`opt` only on reference-kind fields** (`dyn`, `array`, `dict`, struct).
  Those already carry `none` for free (a null `intrusive_ptr` / a `none`
  `EvalValue`). A non-opt **scalar** (`int/float/bool`) has no spare bit to mean
  `none`, and adding a presence flag would break the C layout — so `opt int x;`
  is a v1 error (deferred to §11).

These two restrictions partition structs into two storage kinds, decided once at
decl time (`StructTypeDef::layout`). **POD-ness is recursive:**

- **POD** — every field is a non-opt scalar (`int/float/bool`) **or a non-opt
  POD struct** (embedded *inline*, recursively — §6). Laid out like a C struct
  (offsets/padding/alignment), stored as a **raw byte buffer**, trivially
  copyable; `array<PodStruct>` is stored **flat** (§7). The optimization target.
- **boxed** — has any field that is not POD: an `array`/`dict`/`dyn`/string, a
  **non-POD struct** (held as a *pointer* to a separate instance — a
  refcounted, non-trivial member, which is what makes the parent boxed too), or
  any `opt` field. Stored as a `std::vector<LValue>` slot array;
  `array<BoxedStruct>` is an ordinary general array (works for free).

So a struct-typed field follows its element's nature: a POD-struct field is
**embedded by value** (its bytes inline, parent stays POD); a boxed-struct field
is **a pointer** (parent becomes boxed). A field whose type would make the
struct contain *itself* by value (a size cycle, directly or mutually) is a
compile error — break the cycle with a `dyn`/boxed (pointer) field.

**Scope.** A struct's fields and `const`s live in the **struct's own
namespace**, reached only via `Type.NAME` or `instance.field` — they never
collide with globals or with another struct's members (a struct `const PI` and a
global `const PI` coexist). *Within* one struct, field and const names must be
unique (a field and a const sharing a name is a decl-time error). A `struct`
declaration is a statement allowed **anywhere a `func` decl is**, including
inside a function body — it is lexically scoped exactly like a nested function
(hoisted within its block for forward/mutual reference; the `StructTypeDef` is
AST-owned, so it outlives the call even for a function-local struct used in a
flat array).

---

## 5. The descriptor & the instance object

```cpp
struct FieldDef {
    const UniqueId *name;
    DeclType        scalar_ty;     // b/i/f/arr/dict, or `none` if struct/dyn
    const UniqueId *struct_ty;     // set iff the field is a struct type
    bool            is_dyn;
    bool            is_opt;        // only set for reference kinds (v1)
    int             slot;          // boxed: index into fields vector
    int             offset;        // POD: byte offset; else -1
};

struct StructTypeDef {                 // owned by the StructDeclStmt AST node
    const UniqueId *name;
    std::vector<FieldDef> fields;      // declaration order == ParamSpec order
    std::vector<std::pair<const UniqueId *, EvalValue>> consts;  // folded once
    enum class Layout { pod, boxed } layout;
    int   size;        // POD: total struct size in bytes
    int   align;       // POD: struct alignment
    int   slot_of(const UniqueId *) const;     // field index or -1
    const FieldDef *field_of(const UniqueId *) const;
    const EvalValue *const_of(const UniqueId *) const;
    std::vector<ParamSpec> ctor_params() const;   // {name, is_opt} per field
};

class StructObject : public RefCounted {
    StructTypeDef *def;
    bool readonly;                  // deep-const; nested COW (obj.arr[2]=5)
    union {                         // by def->layout
        std::vector<LValue> fields; // boxed
        std::vector<char>   bytes;  // POD: def->size bytes, C-laid-out
    };
};
```

`ctor_params()` is the bridge to `desugar_named_call` — the **only** thing the
named-arg desugar needs from a struct.

---

## 6. C-style memory layout (POD only)

We adopt C struct *rules* (sequential offsets, per-field alignment padding, tail
padding to the struct's alignment) for compactness and fast field access. We do
**not** need ABI compatibility with the host C compiler (no FFI), which frees us
from arch ABI quirks: the layout is a deterministic function of one number, the
machine word size.

Define our own fixed field metrics so layout never depends on host `alignof`:

```
size_of(bool)  = 1            align_of(bool)  = 1
size_of(int)   = W            align_of(int)   = W   // W = sizeof(int_type)
size_of(float) = 8            align_of(float) = 8   // double, always
size_of(struct)= def->size    align_of(struct)= def->align   // nested POD
```

`W` is 4 (32-bit build) or 8 (64-bit build); `int_type = intptr_t`. On the
target set (little-endian x86/arm/riscv, 32 & 64) these rules give one layout
per word size and nothing else varies. The scalar metric functions are the
*only* place any arch assumption lives, so if some future target needs a tweak
it is a 3-line `#ifdef` there, with all layout logic above it untouched.

Layout pass (at decl time): walk fields in order, `off = roundup(cursor,
align_of(field)); cursor = off + size_of(field)`; `struct.align = max field
align`; `struct.size = roundup(cursor, struct.align)`.

**Nested POD is recursive C nesting.** A POD-struct field contributes its whole
`def->size`/`def->align` at an aligned offset, so its bytes sit **inline** in
the parent buffer (`struct Line { Point a; Point b; }` → a 32-byte `Line` whose
`b` is at offset 16). Field access composes offsets: `line.a.x` loads at
`off(a) + off(x)`; `arr[i].a.x` on a flat `array<Line>` loads at `i*stride +
off(a) + off(x)`, fully unboxed — which is exactly *why* nested POD is required,
not optional (without it a struct-of-structs could never be flat). Layout
therefore has a dependency order (a struct must be laid out after the POD
structs it embeds); the decl-time pass lays each out on first need and reports a
**size cycle** (a value-nesting loop) as a compile error.

A POD struct value is `bytes[def->size]`; a *scalar* field read/write is a typed
load/store at `bytes.data() + field.offset`, a *nested-POD* field is the
sub-range `[off, off+def->size)`. Because POD bytes hold no references, **POD
clone == deepclone == memcpy** and `==` is `memcmp` (same type) — both very fast
and recursion-free regardless of nesting depth.

---

## 7. `array<PodStruct>` — flat storage (the headline optimization)

Add a fifth `SharedObject::Storage` kind, `structs`, alongside
`general/ints/floats/bools`:

```cpp
struct StructVec {                 // the new union member
    StructTypeDef *def;            // the (POD) element type
    std::vector<char> buf;         // count * def->size bytes, contiguous
};
```

Element `i` lives at `buf.data() + i * def->size`. This mirrors `ivec`/`fvec`
exactly, with a struct stride instead of a scalar one. It is built **iff** the
inferencer proved the destination is `array<PodStruct>` — driven by a new
`ArrHint::flat_s` (carrying the `StructTypeDef*`), stamped by
`set_array_repr_hint` the same way `flat_i`/`flat_f`/`flat_b` are. A
`dyn`/heterogeneous/boxed-element destination stays a general array, as today.

Touch the same flat-aware call sites the scalar kinds already have (each already
switches on `skind()`): creation (`builtin_array`, `make_array`, range-n/a,
`LiteralArray`/`LiteralObj`), `clone`/`make_const_clone`/`clone_to_mutable`
(memcpy the buffer + preserve `def`), `size()`, COW (`get_value_for_put`),
`append`/`pop`/`insert`/`erase` (byte splices), `==`/`to_string`, the
idlist/foreach spread, and the subscript read/store. `get_vec()`/`get_view()`
stay general-only (throw on `structs`, the existing invariant tripwire).

`array_storage(a)` reports `"structs"` (tests pin it).

`array<BoxedStruct>` needs none of this — it is a general array whose elements
are `t_struct` values, working the moment structs are a value type.

---

## 8. Construction & access — eval

- **Construction** (`CallExpr::do_eval`): new branch when the callee value
  `is<StructTypeDef*>()` (a `t_structtype`). By that point the call is already
  positional (the desugar ran), so: arity == field count (after desugar an opt
  field that was skipped is an explicit `none`), then per field either store the
  evaluated arg (POD: typed store at the offset, with `coerce_to_decl_type`;
  boxed: `LValue` into the slot, coerced) or, for an omitted opt ref-field, the
  zero value. A named arg on a *function* callee stays the existing error; a
  positional/typed mismatch is caught by the inferencer first.
- **`MemberExpr::do_eval`** dispatches on base type (it already does, for
  dict-vs-error):
  - `t_struct`: `def->slot_of(memId)` → a field (POD: a load, or an lvalue
    view for write/COW; boxed: the slot `LValue`); else `def->const_of` → a
    const rvalue; else field-not-found error. Read-only instance → rvalue only.
  - `t_structtype`: `const_of` only (no instance) → const rvalue; field name →
    "needs an instance"; unknown → not-found.
  - **dict stays as today** (`d.key`) — the original "drop `dict.name`" decision
    is superseded; `.` is overloaded by base type, which the inferencer resolves
    statically (struct base → field slot; dict base → string key) and the
    runtime resolves by tag for a `dyn` base. No grammar conflict.
  - other (int/array/...): `TypeErrorEx` "type has no members" (unchanged).
  - `memId` becomes an interned `const UniqueId *` (resolved at parse time) so
    the slot lookup is pointer compares, not a `SharedStr` compare.
- **Two access paths, kept in agreement.** When the inferencer knows the base is
  a struct (a typed `Point` variable), `obj.field` resolves to a compile-time
  slot/offset (the §10 fast path). When the base is `dyn` (`var dyn d =
  Point(..); d.x`), the type is unknown statically, so `MemberExpr::do_eval`
  reads the runtime tag (`t_struct`) and resolves via `def->slot_of`/`field_of`
  at run time — the same fallback dict access already uses for a `dyn` base.
  Both paths hit the same `StructTypeDef`, so a struct behaves identically
  whether it flows through a typed or a `dyn` variable; the typed path is purely
  an optimization on top.

---

## 9. Type inference & checking (`inferencer.cpp`)

- **Structural pass:** hoist `struct` decls like funcs (forward refs / mutual
  reference work). Bind the struct name to a `TypeSym` whose type is the struct
  descriptor; record the `StructTypeDef` (built by the parser). Resolve a
  struct-typed annotation (`Point p;`) to `Struct(Point)`.
- **`check_call` / `lower_named_args`:** if the callee resolves to a struct
  type, use `def->ctor_params()` as the param list and `field STy`s as the param
  types. Reuse the existing arg-count range, per-arg `assignable` check, and the
  named→positional rewrite. Result type = `Struct(def)`.
- **`MemberExpr` typing:** struct base → the field's static type (or the const's
  type); lets `a[i].x` be a known scalar (drives §10). dict base unchanged.
- **No field fixpoint:** field types are explicit, so they are known after
  parsing — the cost of forbidding `var` fields in v1. Inference *about* structs
  (call results, array element joins, narrowing) uses the normal machinery.
- `--debug-ti` prints struct types by name; `-a` colors a flat `array<Struct>`
  (a new legend entry, analogous to the green flat-scalar arrays).

---

## 10. Fast field access (the M8 analog) — perf phase

After inference proves the static type, specialize hot field access so it never
boxes:

- `s.x` on a typed POD struct → a typed load at `bytes + offset` (reuse the
  `eval_int`/`eval_float` unboxed virtuals from M8; add `eval_bool`-ish via the
  int path). Write `s.x = v` → a typed store.
- `a[i].x` on a flat `array<PodStruct>` → load/store at `buf + i*stride +
  offset` with **no intermediate `StructObject`**. This is the whole point of
  the flat layout and mirrors `Subscript::eval_int` on `array<int>`.

Correctness-first fallback (phases before this): `a[i]` boxes a POD
`StructObject` (memcpy of the element bytes), then `.x` loads from it — correct,
just one copy. The fast path removes the copy.

---

## 11. Deferred (explicitly out of v1, designed so they bolt on)

- **`var` fields** — infer a field's type from *all* construction call sites,
  exactly as function params are inferred from call sites today. The constructor
  *is* a call, so the field list plugs into the same fixpoint contribution
  (`contribute_arg` over `ctor_params`). The blocker is only ordering (a struct
  used before all its constructors are seen); a second fixpoint pass over struct
  fields handles it. Until then: explicit types or `dyn`.
- **`opt` scalar fields** — needs a presence representation that doesn't wreck
  the C layout: either a trailing presence bitmap appended to the POD bytes (the
  struct stays "POD-with-bitmap", still flat-array-able) or demoting such a
  struct to boxed. Pick when implementing; both are localized to layout + field
  load/store.
- **Empty struct** (`struct Unit {}`) — *not* in v1 (rejected at decl time with
  a clear message), but cheap to allow later (size 0, trivially POD). **Keep
  this open** as a possibility; when structs land, this note graduates into
  `CLAUDE.md` next to the struct decl rules so it is not forgotten.
- **Methods**, struct **subtyping**, struct **dict keys / hashing** (v1: not
  hashable, `hash` throws). `==` is field-wise (boxed) / `memcmp` (POD) between
  same-type instances; different types → not equal. `print` shows
  `Point(x: 1, y: 2)`.

(**Nested POD is NOT deferred** — it is required in v1, see §4/§6; it just gets
its own phase/commit in §16.)

---

## 12. const, COW, value semantics

- **`const` works with no limitations.** A struct instance holds no state in the
  *type*, only in the instance, so nothing blocks folding. The descriptor is a
  `const` in scope (registered in `const_ctx`, like a pure func), so
  construction with const args **const-folds at parse time**: extend the
  parser's `CallExpr` const-fold and `MakeConstructFromConstVal` /
  `cse_materialize` to bake a const struct instance into a deep read-only
  `LiteralObj`, exactly as const arrays/dicts are baked. A const struct is deep
  read-only (`StructObject::readonly`, propagated by `make_const_clone`).
  `Point.CONST` / `p.CONST` reads fold to literals at parse time.
- **COW value semantics** like arrays/dicts: assignment aliases; mutating a
  shared instance clones first; `clone()` shallow, `deepclone()` deep; a const
  instance is immutable through any alias (write paths check `readonly` and
  throw `CannotChangeConstEx`). For POD, clone is a memcpy and there is nothing
  to deep-copy.

---

## 13. Parser

- New keyword `struct` (`Keyword::kw_struct = 27`, `kw_count -> 28`; keep
  `KwString` index-aligned).
- `pStmt`: `struct Name { ... }` → `StructDeclStmt`, allowed wherever a
  statement is. Body accepts only `TYPE name;` field decls (reusing
  `pAcceptDeclPrefix` for the scalar/array/dict/`dyn` prefixes, extended to
  accept a **struct-type name** — an `IDENT IDENT` field via one-token
  lookahead) and `const name = expr;` (folded immediately into
  `StructTypeDef::consts`). Build the `StructTypeDef` (fields + slots +
  POD/boxed layout via §6). Reject `var`/untyped fields and `opt` scalar fields
  here with a clear message. Bind the struct name as a `const` descriptor,
  map-resident like a func name (forward refs / nesting).
- **Struct-typed variable decls** outside a struct (`Point p = ...;`): the same
  `IDENT IDENT` lookahead in `pAcceptDeclPrefix` marks a struct-typed
  declaration; the type identifier is validated by the inferencer (it must name
  a struct). `Point(...)` as an expression stays a `CallExpr` (no lookahead
  hazard — `IDENT (` is a call, `IDENT IDENT` is a decl).
- Construction needs **no** new parser code — it is a `CallExpr`, and
  `pTryDesugarNamedCall` gains a struct-descriptor branch (build `ParamSpec`
  from `def->ctor_params()`).

---

## 14. Errors

Reuse where the message fits: `TypeMismatchEx` (wrong field type / not a field /
names through a non-struct), `WrongArgCountEx` (too few/many fields — shared
with `desugar_named_call`), `CannotChangeConstEx` (mutating a const instance).
Add a script-catchable `DECL_RUNTIME_EX` only if a genuinely new runtime
condition appears (e.g. field-not-found on a `dyn` base at runtime —
`TypeErrorEx` likely suffices). Construction-from-const errors propagate at
parse time (not catchable), like every other const-eval error.

---

## 15. Docs impact

- **README**: a new "Structs" section (decl syntax, explicit field types, the
  `opt`-only-on-ref / no-`var` v1 rules, construction = call syntax with
  positional/named/mixed, field & `Type.CONST` access, COW/const semantics,
  `array<Struct>` is flat for POD); note `.` is field access on a struct and
  still key access on a dict; document `array_storage` → `"structs"`.
- **CLAUDE.md**: the two new `TypeE`s + their position vs `t_str`; the
  `StructTypeDef`/`StructObject` model and the **recursive** POD/boxed split
  (nested POD inline vs boxed-struct pointer); the new
  `SharedObject::Storage::structs` + `ArrHint::flat_s`; `MemberExpr` dispatch
  (typed slot/offset vs `dyn`-base runtime path); the const-fold-of-construction
  path; that struct construction *reuses* `desugar_named_call` (the
  constructor-as-call model); the C-layout helpers and the one-place arch
  assumption; struct **scoping** (own member namespace; declarable inside
  functions); and the **empty-struct** note (unsupported in v1, a kept-open
  possibility).

---

## 16. Task order (phased; perf is in-scope, not "maybe later")

Each phase ends green (`-rt`) and is independently committable.

1. **Value types & boxed instances.** `TypeE` ×2, `ValueU`, `TypeNames`/
   `AllTypes`, `TypeStruct`/`TypeStructType` in `struct.cpp.h`; `StructTypeDef`
   (boxed layout only) + `StructObject` (boxed `vector<LValue>`). `==`,
   `to_string`/print, `clone`, `is_true`, `hash` (throw). No parser yet — unit
   tests construct via a temporary hook.
2. **Parser: `struct` decl.** Keyword, `StructDeclStmt`, field decls (explicit
   types incl. struct-type names; reject `var`/opt-scalar), `const` members,
   descriptor binding, `StructTypeDef` build. `-s` shows it.
3. **Construction + named args.** `CallExpr` struct branch; extend
   `pTryDesugarNamedCall` + the inferencer's `lower_named_args`/`check_call` to
   the struct-descriptor callee via `ctor_params()`. Positional, named, mixed,
   arity/type errors — all from the shared path.
4. **Member access + inference.** `MemberExpr` struct dispatch (field/const, the
   typed and `dyn`-base paths of §8), interned `memId`, `STyKind::Struct` typing
   of construction / fields / `Type.CONST`, `array<Struct>` element inference.
   `const` construction folds (deep read-only). COW + const-instance
   immutability. Nested **boxed** structs work here (a struct field is just a
   `t_struct` slot).
5. **C layout (scalar POD).** Detect POD; compute the C layout (§6 helpers, no
   nesting yet); `StructObject` POD byte storage; scalar field load/store at
   offsets; POD clone/`==`/print via bytes. Standalone POD structs now compact.
6. **Nested POD (recursive layout).** *(Dedicated commit, required — §4/§6.)*
   Extend POD-ness and the layout pass to embed a non-opt POD-struct field
   inline (recursive offsets, size-cycle detection); nested-POD field read =
   sub-range; `line.a.x` composes offsets. A non-POD struct field stays a boxed
   pointer.
7. **Flat `array<PodStruct>`.** `Storage::structs` + `StructVec`,
   `ArrHint::flat_s`, `set_array_repr_hint`, all the flat-aware call sites (§7),
   `array_storage` → `"structs"`. `array<Point>` and `array<Line>` (nested) are
   now unboxed.
8. **Fast field access (M8 analog).** Specialize `s.x` / `s.a.x` and `a[i].x` /
   `a[i].a.x` on typed POD structs to direct unboxed load/store (§10).
   Backtrace/behaviour identical; measure on a struct benchmark (add
   `bench/NN_struct_points.{my,py}`).
9. **Tests + docs.** Exhaustive: positional/named/mixed/partial construction,
   nested structs (boxed *and* POD), const via instance & type, COW &
   const-instance immutability, `array<Struct>` storage (`array_storage`), POD
   layout/size + nested-offset assumptions, scoping (struct member vs same-named
   global; a function-local struct), every error (wrong type/arity, unknown/
   duplicate/reordered field, `var`/opt-scalar field rejected, size cycle, names
   through a non-struct), and the perf path. README + CLAUDE.md per §15, folded
   into the phase that introduces each behavior.

---

## 17. Decisions confirmed with the user (this round)

- **Nested POD is required, not optional.** POD-ness is recursive; a POD-struct
  field is embedded by value (inline bytes), a non-POD-struct field is a boxed
  pointer (parent becomes boxed). It gets its own commit (§16 phase 6) but is
  in-scope for v1 (§4/§6). A value-nesting size cycle is a compile error.
- **`dyn`-held struct** behaves identically to a typed one — same
  `StructTypeDef` via a runtime-tag dispatch path; the typed path is just an
  optimization (§8).
- **Empty struct** — *not* in v1 (decl-time error); kept open as a future
  possibility, with a reminder to graduate the note into `CLAUDE.md` when
  structs land (§11). (Open: whether even the v1 *rejection* is worth a test, or
  just leave it unsupported.)
- **Scope** — struct fields/consts are in the struct's own namespace (a struct
  member may share a name with a global; only the qualified `Type.NAME` /
  `instance.field` reaches it). Field/const name collisions *within* one struct
  are rejected at decl time. **Structs may be declared inside functions**
  (lexically scoped like nested funcs). (§4)
