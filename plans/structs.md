# Custom struct types (DEFERRED — design captured for later)

Status: **designed, not started.** Parked in favor of total type inference
(`plans/type-inference.md`). The two features reinforce each other: once
inference lands, struct field access upgrades from a runtime per-type table
lookup to a true compile-time slot index (see "Field access" below).

## Goal

C-like user-defined value types with fields and consts, no methods (v1). Replace
the dict `d.name` member sugar entirely: once structs exist, `.` means
struct-field/const access *only*, and dict access is `d["key"]` (subscript) only.

```
struct Point {
    var x;
    var y;
    const ORIGIN_NAME = "origin";
}

var p = Point(1, 2);          # positional
var q = Point(x: 3, y: 4);    # named
p.x;                          # field read  -> 1
p.ORIGIN_NAME;                # const via instance -> "origin"
Point.ORIGIN_NAME;            # const via type     -> "origin"
```

## Decisions (locked with the user)

- **Access operator: `.` for everything.** `obj.field`, `obj.CONST`,
  `Type.CONST` all use `.`. Rationale: in a dynamically-typed language the
  instance-vs-type distinction that justifies C++ `::` does not exist — `obj.c`
  and `Type.c` resolve through the same mechanism to the same const object. `::`
  would encode a distinction the runtime doesn't make and adds a lexer digraph
  + precedence wiring for no semantic gain. Adding `::` later (if methods/
  statics ever create a real distinction) is backward-compatible, so deferring
  costs nothing.
- **No mixing of named and positional args** in a single constructor call. A
  call is all-positional or all-named, decided at parse time. `Point(1, y: 2)`
  is a `SyntaxErrorEx`.
- **Completeness:** positional must supply *every* field in order (arity
  checked). Named *may* omit fields; omitted fields default to `none`. Matches
  "no defaults" (none == uninitialized).
- **Field access = runtime per-type table lookup, NO cache** (option B). The
  object is a flat slot array; each *type* has a small `name -> index` table
  built once at parse time; `obj.field` does `def->slot_of(uid)` over that tiny
  table (a few interned-pointer comparisons, not a dict, not a per-object map).
  No inline cache (rejected as premature). With total type inference this
  becomes a compile-time constant index.

## Decisions made unless overridden

- Struct instances get COW value semantics exactly like arrays/dicts (assignment
  aliases; mutation clones if shared; `const` instances are deep read-only;
  `clone()` shallow / `deepclone()` deep).
- `==` is structural field-wise between same-type instances; different types ->
  not equal.
- Structs are not hashable (cannot be dict keys) in v1.
- `print` shows `Point(x: 1, y: 2)`.
- Construction is a runtime op in v1 (not const-folded), but const member reads
  `Type.CONST` *do* fold at parse time.
- No methods.

## Value & type model

Two new value kinds, mirroring func's "decl + object" split:

1. `t_structtype` — the **type descriptor**. Holds a raw `StructTypeDef *`
   (trivial pointer, AST-owned/program-lifetime, exactly like `FuncObject`'s
   `FuncDeclStmt *func`). A `const` value bound in scope by the `struct` decl.
   Supports `.CONST` reads and being *called* to construct. Trivial type (placed
   `< t_str`).
2. `t_struct` — an **instance**. Holds `intrusive_ptr<StructObject>`
   (non-trivial, appended after `t_dict`, `>= t_str`).

```cpp
struct StructTypeDef {                 // owned by the StructDeclStmt AST node
    const UniqueId *name;
    std::vector<const UniqueId *> field_names;            // slot i -> name
    std::vector<std::pair<const UniqueId *, int>> slots;  // name -> slot (tiny)
    std::vector<std::pair<const UniqueId *, EvalValue>> consts;  // folded
    int slot_of(const UniqueId *) const;   // linear scan; -1 if not a field
    const EvalValue *const_of(const UniqueId *) const;
};

class StructObject : public RefCounted {
    StructTypeDef *def;
    std::vector<LValue> fields;   // by slot; LValue carries container back-ptr
    bool readonly;                //   for nested COW (obj.arr[2] = 5)
};
```

## Parser

- Add `struct` keyword (Keyword enum slot 24 + KwString, kept index-aligned).
- `pStmt`: `struct Name { ... }` -> `StructDeclStmt`. Body accepts only
  `var field;` and `const name = expr;`. Consts are const-folded immediately
  into the def; fields get sequential slots. The struct name binds as a `const`
  descriptor — map-resident like a func name (so nested funcs / forward refs see
  it). Struct decls allowed wherever a statement is (like func).
- Named args: give `ExprList` a parallel `std::vector<const UniqueId *> names`
  (empty => all positional; cloned in `ExprList::clone`). In `pAcceptCallExpr`,
  per argument peek `id ':'` (LL(2)) to capture a name; else parse positionally.
  Enforce no-mixing at parse time.

## Eval

- `MemberExpr::do_eval` dispatches on the base type:
  - `t_struct` instance: `def->slot_of(memId)` -> field (COW + lvalue, or rvalue
    for a read-only instance); else `def->const_of(memId)` -> const rvalue; else
    a field-not-found error.
  - `t_structtype` descriptor: only `const_of` (no instance) -> const rvalue;
    field name or unknown -> error.
  - anything else (dict, int, ...): `TypeErrorEx` "type has no members". This is
    where `dict.name` dies.
  - `memId` stored as an interned `const UniqueId *` (resolved at parse time),
    not a `SharedStr`.
- `CallExpr::do_eval`: new branch `callable.is<StructTypeRef>()` -> construct a
  `StructObject` (positional arity check, or named fill with `none` defaults;
  named args on a *function* call -> `TypeErrorEx`).
- New `TypeStruct` / `TypeStructType` classes in `src/types/struct.cpp.h`
  (`#include`d into `types.cpp`): `to_string`, `eq`, `clone`, `is_true`, `hash`
  (throw — not hashable), plus member machinery. Wire `TypeE`, `TypeToEnum`,
  `ValueU`, `TypeNames`, `AllTypes`.

## Dropping `dict.name` — migration

- `MemberExpr::do_eval` no longer accepts dicts (falls to `TypeErrorEx`).
- README: rewrite lines ~150-152 and ~462 (the "member of" perk); CLAUDE.md: the
  "`d.key` auto-vivifies" note under "Evaluation specifics".
- Tests/bench to migrate to `d["key"]`: bench `25_dict_member.my` (rethink —
  convert to a struct benchmark or to subscript) and `48_const_fold.my`; ~40
  test lines in `tests.cpp` (~3970-3990, 1288, 1484-1485). The `x.foo`-on-int
  test (line ~1896) already expects `TypeErrorEx` and stays valid.

## Errors

New `DECL_RUNTIME_EX` types (script-catchable): a field-not-found error and a
construction (wrong-args) error, or reuse `TypeErrorEx` / `InvalidNumberOfArgsEx`
where the message fits.

## Task order

1. `StructTypeDef` + `StructObject` + the two `TypeE`s and their `Type` classes.
2. Parser: `struct` decl + `StructDeclStmt` + descriptor binding.
3. Named-arg parsing (`ExprList::names`).
4. `MemberExpr` rewrite (struct dispatch; drop dict).
5. Construction in `CallExpr`.
6. Drop `dict.name`: migrate tests/bench, update README + CLAUDE.md.
7. Tests: positional/named/partial-named, nested structs, const via instance &
   type, COW/const-instance immutability, errors.
