# The cast: `cast<T>(x)`

**Status: design decided by the maintainer (2026-11-19). Built FIRST,
before the strict container rules of `strict-container-types.md`, which
then use it wherever a program needs a conversion.** It is a new
language feature, decided as such.

## 1. The rules

### 1.1 Syntax and result

`cast<T>(x)`: `cast` is a keyword, `T` any COMPLETE type, parsed as a
declaration's type is (scalars, `?`, struct and class names, type
aliases, `box<...>`, `array<...>`, `dict<...>`, nested). A hole - the
bare `array` / `dict` (`cast<array>(x)`) - is refused at compile time.

The result's static type is exactly `T`, and at run time the value has
exactly type `T` (a container carries `T` as its own type, Q2). The cast
is a RUN-TIME operation: it converts and checks the value as it goes.

### 1.2 Scalars

| to \ from | `bool`     | `int`        | `float`        | `str`         |
|-----------|------------|--------------|----------------|---------------|
| `bool`    | same       | 0 / 1 only   | 0.0 / 1.0 only | the words     |
| `int`     | 0 / 1      | same         | = `int(f)`     | = `int(s)`    |
| `float`   | 0.0 / 1.0  | = `float(i)` | same           | = `float(s)`  |
| `str`     | = `str(b)` | = `str(i)`   | = `str(f)`     | same          |

- `= int(f)`: truncates; NaN, an infinity and an out-of-range float are
  refused, as `int()` refuses them. `= int(s)` / `= float(s)`: the whole
  string is the number, as `int()` / `float()` read it (`numtext.h`).
- str -> bool, ignoring case: `true` / `yes` / `on` / `enabled` give
  `true`; `false` / `no` / `off` / `disabled` give `false`. Nothing
  else, no surrounding spaces (as `int(s)` takes the whole string).
- A container cast to `str` is refused (`str(x)` still prints one).

### 1.3 `none`, `T?`, `dyn`, `dyn?`

- `T?`: `none` passes; any other value is cast to `T`.
- `none` into a non-`?` type is refused.
- `cast<dyn?>(x)` always succeeds: the value is kept as it is (no
  conversion, no copy), only the static type changes.
- `cast<dyn>(x)` is the same plus a run-time check that `x` is not
  `none`.

### 1.4 Containers

`cast<array<E>>(a)` / `cast<dict<K, V>>(d)` build a NEW container of
exactly type `T` (storage following `T`: `array<int>` flat, `array<dyn>`
general), converting each element - key and value - by these rules,
recursively.

- Same type: a SHALLOW copy, as `clone()` makes.
- A nested container is rebuilt only where its type differs from the
  target's; one that already has the target type is shared, as a
  shallow copy shares it. Under a `dyn` / `dyn?` position the value is
  kept as it is (`dyn` refuses `none`).
- A dict keeps its insertion order and its default value (cast too).
- Two keys that cast to the same key (`"1"` and `"01"` -> `1`, `1.2` and
  `1.7` -> `1`) are refused, naming both: a dropped entry would be
  silent.
- An array cannot become a dict or the reverse.

### 1.5 Structs, classes, boxes

The value must already be that struct, that class, that `box<...>`
(a `dyn` holding one is checked at run time). The result is a shallow
copy: a struct is a value anyway; a class instance comes back as a NEW
instance with the same field values; a box as a new box. (The way to
check a `dyn` without copying stays a typed place: `C c = d;`.)

### 1.6 Failures

- A value the conversion cannot take: **InvalidValueEx** (as `int()` /
  `float()` raise): `"abc"` to `int`, `2` to `bool`, NaN to `int`, two
  keys colliding.
- A value of the wrong KIND: **TypeErrorEx**: an array cast to `int`, a
  struct `Q` to `P`, `none` into a non-`?` type.
- The message names the position inside the value (`cast<array<int>>:
  [2] "abc" is not an int`); the caret is the cast's argument.
- The result is complete before anyone sees it: a failure leaves the
  source untouched and no partial result anywhere.

### 1.7 Compile time

- A cast that can NEVER succeed is a compile error (TypeMismatchEx):
  the static source type is known and no value of it converts to `T` -
  `cast<int>(xs)` with `xs` an `array<int>`, `cast<array<int>>(5)`,
  `cast<array<array<int>>>(xs)`, `cast<P>(q)` with `q` a `Q`,
  `cast<dyn>(none)`. A `dyn` / unknown source is decided at run time.
- A cast of a constant folds at compile time, so a failing one
  (`cast<int>("abc")`) is a compile error, as `int("abc")` is today
  (question 6's rule). Folding must give exactly the run's value and
  type (RULE 2): `-nc` is the oracle.

### 1.8 `dynarray()` is removed

`cast<array<dyn>>(a)` / `cast<array<dyn?>>(a)` replace it everywhere
(tests, samples, README, `:help`).

## 2. Implementation

One conversion function shared by every engine, so they cannot drift:

1. **Runtime core** (types/cast.cpp.h, included by types.cpp beside
   fit.cpp.h): `cast_value(const EvalValue &v, const TypeAnnot *t, ...)`
   builds the result along `t` (scalar table, words, containers with
   their storage and interned `ctype`, key-collision check, struct /
   class / box copies), throwing with the position path. A cycle in the
   value is walked with the cycle guard (`cyclewalk.h`): a container met
   again maps to its one converted copy, as the deep copies do.
2. **Lexer / parser**: `kw_cast`; in `pExpr01`, `cast` `<` TypeAnnot
   `>` `(` expression `)` -> a new `CastExpr` node (`ConstructType::
   cast`, a `NodeTag` row, `clone`, `serialize`). The closing `>>` of a
   nested type goes through `pAcceptCloseAngle`. A hole is refused.
   `cast` stops being usable as a name (the corpus has none).
3. **Every walker classifies the node**: the exhaustive switches
   (`-Werror=switch`) in the inferencer's and resolver's child walkers,
   `esc_known_shape`, the callee-set `cs_eval` (a `dyn` / `dyn?` cast
   passes its operand's set; a scalar target has none; any other is ⊤),
   coderender (renders `cast<T>(x)`), and the analysis passes.
4. **Inferencer**: `type_of` = T; the check pass refuses a never-
   possible cast (1.7); `th` i/f for a non-`?` int / float target, so
   typed code reads the result unboxed; `ctype` stamped with T.
5. **Tree-walker**: `CastExpr::do_eval` calls the core.
6. **VM**: a `CastV` opcode (dst, source slot, T as a ref into
   `Chunk::ctypes`), appended to `ML_FOR_EACH_OPCODE`; rows in
   `visit_use_def`, `op_writes_scalar`, `verify_chunk`, the
   `opcode_table_census` columns, `int_op_result_kind`; `-vd` renders
   it. A typed (th i/f) consumer reads the slot as the other boxed
   producers' results are read.
7. **JIT**: a conveying status helper (`jit_cast`, `extern "C" int`,
   exc-stamped from the loc table), reached through the call seam.
8. **`.myv`**: the new opcode bumps `MYV_FORMAT_VERSION`;
   docs/myv-format.txt and `tests/myv_doc_check` updated.
9. **Builtins**: `dynarray` deleted (the builtin-set fingerprint
   changes with it, covered by the version bump).
10. **REPL / editor**: `cast` in the highlighter's keywords, the
    completion keywords, a `:help cast` entry.

## 3. Tests

- `-rt` (five modes, and a caret check per failure kind): every cell
  of the scalar table, the words in mixed case, `T?` / `dyn` / `dyn?`,
  arrays and dicts of each element kind, nested rebuild vs sharing,
  same-type shallow copy (identity checked with `intptr`), key
  collisions, struct / class / box, a cyclic value, the compile-time
  refusals, a constant cast folding (and failing at compile time), and
  `-nc` equivalence.
- `tests/functional/NN_cast.my`: the shapes under every engine and JIT
  lever (corpus_diff), including casts inside JIT-compiled loops.
- A `tests/int` coverage unit for what the above does not own; value
  classes for the boundaries (int -> bool at 0 / 1 / 2, float -> int at
  the int limits) if the coverage run asks for them.
- `driver_checks` / image round trip: the functional program through
  `.myv`.

## 4. Docs

README: a *Casts* section (the table, the words, containers, failures),
`dynarray` removed from the builtin list; CLAUDE.md: `CastExpr`,
`CastV`, `cast_value` as THE conversion; docs/vm-ops.md: `CastV`.

## 5. Then

`strict-container-types.md` section 6: measure, the strict rules (using
the cast where a program converts), then the template extensions.
