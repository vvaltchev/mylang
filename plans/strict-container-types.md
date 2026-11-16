# Strict container types, a non-null `dyn`, and the cast

**Status: rules decided by the maintainer (2026-11-18/19), nothing built
yet.** This replaces question 7 of `typed-containers.md` (7.1 and 7.2)
and tightens its decision A. Two points are still open (section 3).
The cast is designed in `cast.md` (section 5); it is built FIRST, so
the strict rule never needs a hand-written conversion loop.

## 1. The goal

A name that is not `dyn` has a type the compiler knows exactly, and the
value it holds has exactly that type at run time. So typed code needs
no run-time type check, and the JIT may trust every static type. The
only run-time checks left are where a `dyn` value enters a typed place
(checked once, there) and the operations on `dyn` values themselves.

The maintainer's principle: **no implicit expensive conversions**.
Re-creating a whole container is expensive and is not aliasing, so it
happens only when the program asks for it, with the cast.

## 2. The rules (decided)

1. **Container types are exact.** No implicit conversion between two
   different container types, in any direction, at any depth:
   `array<int>` / `array<dyn>`, `array<T>` / `array<T?>`, `array<dyn>`
   / `array<dyn?>`, `array<int>` / `array<float>`, the same for dicts,
   the same nested. A conversion the compiler can see is a compile
   error. A `dyn` value entering a typed place is checked at run time
   against the place's exact type. This removes increment 3's rule that
   a `dyn` component accepts a narrower container.
2. **`array<dyn>` is a fixed type**, like `array<int>`: an array of
   variants (general storage) that are never `none`. It is not a
   placeholder for `array<T>`. `array<dyn?>` is the array of variants
   that may be `none`. The two are different types (rule 1).
3. **`dyn` is non-null everywhere**: variables, elements, fields,
   parameters. `dyn?` (= `opt dyn`) may hold `none`.
   1. Compile time: `dyn a = none`, `a = none`, and assigning anything
      possibly-none (a `T?`, a `dyn?`, a function that may return none)
      to a `dyn` variable, element or field is a NullabilityEx, as for
      `int a = none`. Inference no longer widens a `dyn` that receives
      a none to `dyn?`.
   2. `runtime(x)` takes its argument's nullability: `runtime(none)` is
      `dyn?`, `runtime(1)` is `dyn`.
   3. The run-time check at a parameter's bind stays (it is what an
      untyped run, `-nti`, relies on); an untyped store into a `dyn`
      place gets the same check.
4. **Storage follows the type.** Every `array<int>` / `array<float>` /
   `array<bool>` is flat and every `array<dyn>` general, so the JIT can
   drop the storage-kind dispatch for typed arrays. Today an
   `array<dyn>` can hold a flat array (by rule 1's aliasing, gone with
   it), and an inline `keys()` or `dynarray()` can make a general
   `array<int>`: those must go too.
5. **The bare `array` and `dict` annotations are a HOLE the compiler
   fills, never a type of their own** (not aliases of `array<dyn?>`):
   1. in a declaration, filled once from what the variable receives:
      `array a = [1, none]` stays `array<int?>`;
   2. in a parameter, filled per call: the function is a template, as
      if the parameter were un-annotated, plus a compile-time check
      that the argument is an array (dict). Today such a parameter
      joins its call sites into one type instead - `array<int>` and
      `array<str>` gave `array<dyn>` (aliasing, forbidden by rule 1),
      `array<int>` and `array<float>` a refused argument;
   3. in a struct field: forbidden, a full type is required.
   Nested holes (`array<array>`) may come later; the top level first.
6. **`none` is a valid dict key.**
7. **Scalar widening stays implicit** (int -> float, bool -> int /
   float): it copies one value and aliases nothing. (Assumed in the
   discussion; confirm when the cast's rules are agreed.)
8. **More functions become templates**, so a function called with two
   array types gets a typed copy per type instead of a join (which rule
   1 refuses):
   1. an un-annotated `opt` parameter is a template parameter (keyed by
      its non-none type); today it joins;
   2. a capturing closure, and a lambda used as a value, get instances:
      the variable still holds ONE closure object (one capture vector -
      captures are shared, mutable state), its descriptor holds the
      list of instance bodies, and each call site that can name the
      lambda picks its instance at compile time from the static
      argument types. A call site that cannot name it calls the untyped
      base body. Until this lands, `func [c] (~a)` is the spelling.
   Related: question 15 (`closure-param-join.md`).

## 3. Open

1. **The join of two different container types** (`cond ? names :
   maybe`, a variable assigned both, a function returning either). No
   container type holds both without a conversion. Recommended: the
   join is `dyn` (free, aliasing, stores checked at run time), which
   the mandatory-`dyn` rule makes the program write (`var dyn c`).
   Alternative: a compile error.
2. **A call whose callee the compiler cannot name** (one of several
   lambdas chosen at run time; a template called through a `dyn`).
   Today the parameters silently become `dyn`, or the untyped template
   base runs. Recommended: keep that implicit `dyn`. Alternative:
   require the parameter to be written `dyn`.

## 4. What happens today (measured 2026-11-18/19, every engine)

```C#
array<int> xs = [1, 2];
array<str> ss = ["a"];
```

- `array<dyn> a = xs;` compiles and ALIASES `xs`: same `intptr`,
  `a[0] = 7` shows in `xs`, `a[1] = "s"` is refused at run time (the
  array is still an `array<int>`).
- `array<int> w = q;` (`q` an `array<dyn>`) compiles and is refused at
  run time: "'w' is declared 'array<int>' but the value is an
  'array<dyn>'".
- `var c = runtime(true) ? names : maybe;` (`array<str>`,
  `array<str?>`) makes `c` the `names` array typed `array<str?>`, so
  `c[0] = none` stores a none into an `array<str>` unchecked. Then
  `len(names[0])` raises TypeErrorEx in the tree-walker and the VM and
  prints `0` under the JIT: a RULE 1 hole and a RULE 2 divergence.
- `var d = cond ? xi : xf;` makes `d` the flat-int `xi` typed
  `array<float>`; a typed read converts each element, a float store is
  refused by the flat storage.
- `dyn a = none;`, `dyn a = 1; a = none;`, a possibly-none function
  result or a `dyn?` into a `dyn`, `a = runtime(none)`: all accepted.
  `array<dyn> a = [1, none]` and `a[0] = none` too. Only a parameter
  separates them: `func f(dyn x)` called with `none` is an
  OptRequiredEx, with `runtime(none)` a TypeErrorEx at the bind.
- `func f(array a)` joins its call sites (above); `struct S { array a;
  }` makes the field an `array<dyn>`.
- Joining parameters (one body for every call): a capturing closure, a
  lambda stored in an array, a named function with an `opt` parameter -
  each typed `array<dyn>` (or `array<dyn>?`) when called with `xs` and
  `ss`. A named template passed as a value (`apply(total, xs)`) does get
  instances.

## 5. The cast

Decided 2026-11-19: `plans/cast.md` (syntax, the conversion table, the
words `str` -> `bool` accepts, containers, failures, implementation).

## 6. Order

1. The cast (section 5) - built 2026-11-19.
2. Measure, before changing any rule: which corpus programs and tests
   each of rules 1, 3 and 5 refuses (a compile run of tests/functional,
   tests/int, samples, bench/my and the `-rt` sources).
3. Rules 1-7, fixing the programs with the cast; decision A's
   implementation (`ct_enters`) and its tests change with rule 1.
4. Rule 8: `opt` parameters as template parameters, then closure
   instances.
