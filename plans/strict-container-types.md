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
   tests/int, samples, bench/my and the `-rt` sources) - done
   2026-11-20, section 7.
3. Rules 1-7, fixing the programs with the cast; decision A's
   implementation (`ct_enters`) and its tests change with rule 1.
4. Rule 8: `opt` parameters as template parameters, then closure
   instances.

## 7. Measured (2026-11-20): what rules 1, 3 and 5 refuse

A measurement build (not committed) prototyped rule 1 NAIVELY - a `dyn`
component no longer fits another type, and two settled container types
no longer join - and rule 3 - a non-opt `dyn` refuses a possibly-none
value, and a `dyn` variable that receives one is refused. `-nr` ran over
389 programs (tests/functional, tests/int, tests/backtrace, bench/my,
samples; 17 skipped, which need an INT build), and `-rt` ran under each.

**Rule 3 refuses 4 programs and ~18 `-rt` cases, all genuine**: a `var
dyn x` that receives a possibly-none value, which becomes `dyn?`.
samples/shopping (`var dyn found = find(...)`), 22_closure_store (`var
dyn bf = b.fn`, a `dyn?` field), 72_none_bind (`var dyn i = find(...)`),
45_static_result_types (`var dyn tq = yes ? oa : 7`, `oa` an
`array<int>?`). The `-rt` cases are the same shape (`var dyn a = none; a
?? 1`), plus the cases that assert today's widening ("ti dyn accept: dyn
var infers opt from none-flow") - they change to assert the refusal.

**Rule 5 changes almost nothing.** No program in the corpus has a bare
`array`/`dict` struct field (`-rt` has 7, `struct P { int x; array t;
}`, which become `array<dyn?> t` or a full type); two `tests/int`
parameters (`func garr(array a)`) become templates.

**Rule 1, prototyped naively, refuses 32 programs and ~40 `-rt` cases,
and almost none of them is a program that converts a container.** They
are places where the COMPILER builds a container type the naive rule
refuses:

1. **A `dyn` value stored into a typed container** - `a[i] = d`,
   `append(xs, runtime(6))`, `[P(5, 5), db]` into an `array<P>`: the
   store contributes `array<dyn>` to the variable, which no longer fits
   `array<int>`. Rule 1 says such a value is checked at run time where
   it enters, so the store must contribute an ELEMENT (checked when it
   is `dyn`), not a container type. 12 programs (the tests/backtrace
   callback and foreach programs, 103, 93, 75, 47, 117, 45, 36, 98) and
   most of the `-rt` cases (#49's misfit carets, "a builder for a proven
   array<int> refuses a dyn string").
2. **A scalar widened into an element** - `fa[0] = 3` into an
   `array<float>`, `u[0] = n` in a pure body: rule 7's widening, refused
   because the store contributes `array<int>`. The same fix as 1 (70,
   73, "a bool WIDENS into int/float storage").
3. **A builtin's unknown component typed `dyn` instead of unfilled** -
   `dict(0)` is `dict<dyn,int>` (bench/my 47 and 62, 30, 90, 85, 132,
   44), `dict(pairs)` `dict<dyn,dyn>` (118). They become `dict<none,
   int>`, the key unfilled, and the stores fill it.
4. **A later store that changes an inferred variable's element type** -
   `var wx = [1]; wx[0] = 2.5` (today `array<float>`, the literal built
   in float storage), `append(gs, "s")` on an `array<dyn>`, `[[1],
   ["s"]]` into `array<array<dyn>>`. Legal under the rules when the
   literal takes the variable's type, as it does today (129, 128, 126).
5. **Values of two types in one fresh container** - `append(r, r)`
   (124), `n["c"] = 5` into `{"a": {...}}` (int 128), box<int> and
   box<P> keys (40): today `array<dyn>` / `dict<str,dyn>` /
   `dict<dyn,str>`, built so from the start - no conversion, so legal.
6. **Array `+` of two element types** (125): the naive join returns
   null and `binop_result` asserts. The result is a fresh array; its
   type is an open point below.
7. **A test passing an `array<dyn>` where an `array<int>` is declared,
   expecting the run-time refusal** (foreach_typed_misfit: `ints(cast<
   array<dyn>>([1, 2]))`): refused at compile time now; the test passes
   the value through a `dyn` name.

So the implementation of rule 1 is mostly a change to how the
inferencer contributes to a container, and the corpus barely changes.
**Proposed design (to confirm):**

- A container VALUE that flows from one place to another (an
  assignment from a variable, an argument, a return, a ternary arm)
  must have the destination's exact type. Two settled different types
  meeting in one place is open point 1.
- An element contribution (a store, an `append` / `push` / `insert`, a
  literal's element) is checked against the element type: a scalar
  widens (rule 7), a `dyn` value is checked at run time (as a `dyn`
  into a numeric local is today), anything else is a compile error at
  the store - `var a = [1]; var b = a; b[0] = "s"` is refused AT THE
  STORE, where today `b` silently becomes `array<dyn>` while it aliases
  `a`'s flat int storage (the hole of section 4).
- Except for a FRESH container: a variable every one of whose container
  values is a literal or a builtin's new container takes the join of
  its element contributions (mixed kinds -> `dyn`, numbers widen), and
  its literals are built in that type - what happens today, and no
  conversion. `var a = [1]; a[0] = "s"` stays legal (`array<dyn>`).
- A builtin result's unknown component is unfilled (`none`), never
  `dyn`.

New open point found by the measurement:

3. **`a + b` of two arrays of different element types.** The result is
   a new array, so no aliasing; but its type is a join. Recommended:
   the element join (`array<int> + array<float>` is `array<float>`, the
   ints widened as they are copied; `array<int> + array<str>` is
   `array<dyn>`). Alternative: a compile error (cast one side first).

## 8. Provisional choices (Claude, 2026-11-20 - for the maintainer to confirm)

The maintainer was away and asked for reasonable choices, to be reviewed
later. Each is marked with what it would take to reverse it.

**Choices 1-3 CONFIRMED by the maintainer, 2026-11-27**, with one
direction for 2: the compiler names the function being called every time
it can - only a call whose callee depends on a run-time value (several
candidates, which run their bases - rule 8.2's decision - and the JIT's
guard chain makes fast) or that the program hides on purpose stays
unnamed. Done the same day: the callee-set analysis models the builtins
whose effect is known (the reads-only list, append / push / insert, and
the builtins whose result comes out of their first argument), so
`len(fs)` no longer makes `fs[i](x)` an unknown callee (CLAUDE.md, the
callee-set section). What still has no name: `runtime(f)`, a caught
payload, a `dyn` parameter fed by one of those, the result of any other
builtin, a function a builtin calls back, a REPL closure from an earlier
input, more than CS_MAX_SET candidates, a call through a builtin held in
a variable. And a mixed component keeps its nullability (`[none, 1,
"s"]` is an `array<dyn?>`, as `[1, "s", none]` was - the order used to
decide, and a `dyn` element held the none).

1. **Open point 1 (a join of two container types): the recommended
   answer, `dyn`.** The program writes `var dyn c` (mandatory dyn).
2. **Open point 2 (an unnameable callee): the recommended answer, the
   implicit `dyn` stays.**
3. **Open point 3 (`a + b` of two element types): the element join.**
   `array<int> + array<float>` is `array<float>`, `array<int> +
   array<str>` is `array<dyn>`; the result is new, nothing aliases.
4. **The design of section 7 (fresh containers keep joining) is taken.**
5. **`dyn x;` with no initial value is refused**: `dyn` has no zero
   value, and rule 3 forbids `none`. `dyn? x;` starts as `none`.
6. **`array(n)` fills the element type's ZERO VALUE** (Go's `make`):
   `""` for a `str`, a fresh `[]` / `{}` for a container, a zero struct,
   as `T x;` does - where it filled `none` into elements typed `str`,
   which the engines then read three different ways (below). An element
   type with no zero value (`dyn`, a function, a struct with a non-opt
   `dyn` field) is a compile error for `array(n)`: write `array(n, v)`
   or give the element type `?`.
7. **An explicit `none` element is a `none`, not "no element"** (the
   question 9 rule kept for an EMPTY container and for `array(n)`):
   `var a = [1, 2]; a[0] = none` makes `a` an `array<int?>` (it was a
   run-time TypeErrorEx from the flat storage), and `[none]` is an
   `array<dyn?>` (amended 2026-11-21: it said "until filled", and a
   container holding only nones was typed UNFILLED - `var a = [none,
   none]; a[0] = 5` made an `array<int>` holding a none, and `var d =
   {none: 1}; d[2] = 5` a `dict<int,int>` whose none key the JIT read as
   an int while the VM aborted and the tree-walker threw. Such a
   component is `dyn?` now, wherever it comes from - a literal, a baked
   one, `array(n, none)`, a callback that always returns none - and a
   store keeps it `dyn?`; a literal written for a typed place,
   `array<int?> a = [none]`, still has the place's type).

8. **A container made where `dyn` is declared is an `array<dyn?>`**
   (`dict<dyn?, dyn?>`): the DYNAMIC type, which refuses nothing (it was
   `array<dyn>`, which rule 2 makes the fixed type of non-none
   variants - so `var dyn d = [1, none]` held a `none` its own type
   refused). Consequence: such a container no longer enters an
   `array<dyn>` parameter (rule 3: an `array<dyn?>` would let a `none`
   in); `cast<array<dyn>>(d)` converts it. Reversing it means choosing
   the dynamic type from the literal's own nullability instead (`[1,
   2]` an `array<dyn>`, `[1, none]` an `array<dyn?>`).

9. **A multi-assignment's literal builds each element for its target**
   (rule 1, increment 3): `a, b = [[d], [d]]` with `array<int> a, b`
   types each element literal as its target, exactly as `a = [d]` does
   - a literal is unpacked as written, so each element IS the value its
   target receives. Without it the same program refused the
   multi-assignment form and accepted the single one.
10. **A typed literal refuses a misfitting `dyn` element while it is
    built**, with the numeric coercion's message ("cannot store a
    non-int value in an 'int' variable"), in every engine: the VM's
    `CoerceNumV` already did, and the tree-walker's literal widening is
    checked the same way now. The error is the literal's caret, before
    anything is stored (a multi-assignment writes no target). The fit
    check's own wording ("'v' is declared 'array<int>' but the value's
    [0] is a 'str'") would read better; getting it would mean an
    unchecked widening op in the VM - left for later.
11. **A possibly-none value (`T?`, `dyn?`) into a non-opt element or
    dict value is a compile-time NullabilityEx**, as it already was into
    a field. A `dyn?` into an element that may be none fits (checked at
    run time), as a `dyn` does into any element.

12. **A container a name may hold is refused where it FLOWS, not at the
    store that widens it.** `var b = a; b[0] = "s"` is a compile error
    at `var b = a` ("'b' has type 'array<dyn>' but the container it is
    given is an 'array<int>'"), as is a `return` or a ternary arm of
    such a value into a joined result. The design's wording was "at the
    store"; the flow is where the two types meet, and one check covers
    every store, append and `+=` that could widen it. Reversing it means
    tracking, per variable, which contributions came from an alias and
    refusing the store instead.
13. **`dict(pairs)` is typed by its pairs** (`array<array<T>>` gives
    `dict<T, T>`), not unfilled: its keys come from the pairs, so an
    unfilled key a later store filled would describe a dict holding
    other keys - measured: the JIT read a string key as an int.
    `dict(v)` (a default dict, empty when made) has its key unfilled.
14. **A template is instantiated only for a signature whose container
    components have settled.** An `array<?>` instance refused what its
    argument settled to (exact types); a literal with an unsettled
    element is unsettled itself.
15. **A `dyn?` value given to a plain variable makes it nullable**
    (2026-11-21): a numeric local keeps its type, nullable (`var s = 0;
    s = d` with `d` a `dyn?` is an `int?` - it was an `int` holding the
    none, which the JIT read as 0, the VM aborted on and the tree-walker
    threw at), anything else is a `dyn?` (a foreach variable over
    `dyn?` elements was a `dyn`).
16. **Rule 8.1's key for a `none` argument** (2026-11-21): rule 8.1
    keys an `opt` template parameter by the argument's non-none type, and
    a `none` (or an omitted argument) has none - so it is an instance of
    its own, where the parameter is a `dyn?` (a `none` contributed to an
    `opt` parameter adds nothing). The alternative, reusing the one other
    instance when there is exactly one, makes which instance a call runs
    depend on the program's other calls.

### A gap left by increment 1

A `dyn` READ through a `dyn` (`var dyn n = holes[1]`, where `holes`
is a `dyn` holding `[1, none]`) can hold `none` at run time: no store
check runs on a variable (rule 3.3's "an untyped store into a `dyn`
place gets the same check" covers a container's element, a dict value
and a field, all checked against the container's own type; a VARIABLE
has no run-time type to check against). Such a value is caught where it
next enters a typed place - a parameter's bind, a typed store - as
before.

### A bug found on the way (pre-existing, RULE 1 and RULE 2)

`var a = array(2); a[0] = "x"; print(len(a[1]));` - `a` is inferred
`array<str>` while `a[1]` holds `none`: the tree-walker raises
TypeErrorEx with a caret, `-nj` raises one with NO caret, and the JIT
prints `0`. Choice 6 fixes it.

## 9. Increments

1. Rule 3 for variables, fields and `runtime()`; choices 5 and 8. DONE
   2026-11-20.
2. Choice 6 (`array(n)`'s zero values). DONE 2026-11-20. Measured: one
   `-rt` case refused (`func f(n) { var a = array(8); ... a[1] += n; }`
   called with `runtime(4)`: the instance's elements are `dyn`, which has
   no zero value - it now writes `array(8, false)`); every corpus
   program compiles. In a template's BASE body (not checked in
   isolation) an element type with no zero value makes the array
   untyped and none-filled instead of a refusal. Left for increment 3:
   an `array(n)` no VARIABLE types - returned from a function, then
   filled by the caller (`func mk(n) => array(n); var q = mk(2); q[0] =
   "s";`) - is still filled with none and typed `array<str>` by the
   caller's store, the three-engine divergence above (the tree-walker's
   caret, -nj's caret-less TypeErrorEx, the JIT's 0). Its fix is the
   NULLABLE unfilled element type that choice 7 introduces: such an
   array is an `array<dyn?>` until a variable types it.
3. Rule 1: element contributions, fresh containers, exact assignment;
   decision A's `ct_enters` loses the `dyn` component's tolerance; and
   choice 7. DONE 2026-11-21 (choices 7, 9-14 above, for the
   maintainer): exact static and run-time container types, the flow
   check (a container a name may hold keeps its type into a variable, a
   result, a ternary), the element contributions (a `dyn` keeps a
   numeric element, a `none` makes it nullable), the array sum's element
   join and storage, `dict()` typing, the template signature rule. The
   corpus compiles as before (0 of ~390 programs change outcome); the
   tests that used an `array<dyn>` name for a value the compiler cannot
   see go through a `dyn` name. **Still open:** the increment 2
   leftover above - `func mk(n) => array(n); var q = mk(2); q[0] =
   "s";` types `q` an `array<str>` holding none (the flow check lets an
   unfilled component match, which is sound for an EMPTY container and
   not for `array(n)`'s none-filled one). The fix needs `array(n)` with
   no type of its own to be an `array<dyn?>` once it flows out untyped,
   while `var a = array(2); a[0] = "x"` keeps typing `a` by its stores.
   **DONE 2026-11-27 (item 1, the maintainer chose `array<dyn?>` over
   a compile error):** `decide_array_n`, after the program has
   settled, marks every `array(n)` whose own function leaves its element
   type open - no variable whose type has an element and no DECLARED
   place (a declared parameter, a field, a declared container's element
   or append, through `?:` / `??` arms) decides it: returned, passed to a
   parameter nothing declares, an element of a new literal, a variable
   nothing types - and the fixpoint runs again with those typed
   `array<dyn?>`. A declared place gets its zero values (it got nones,
   which the bind refused at run time). A callee's stores through an
   undeclared parameter do not decide it (`var b = array(3); fill(b);`
   is an `array<dyn?>`). Measured: 0 corpus programs change outcome; one
   `-rt` row (`typestr(array(2))`) now says `array<dyn?>`. A read of such
   an element into a plain `var` needs `var dyn?`. Tests:
   tests/functional/150, the `strict item 1:` `-rt` rows.
   **Also open (found 2026-11-21) - DONE 2026-11-27, below:** the flow
   check covers a variable, a result and a ternary arm, not an ELEMENT: a container a name holds,
   put into an inferred container whose element type later widens -
   `var r = [cs]; append(r, r);` with `cs` an array<C> - makes `r` an
   array<array<dyn>> holding `cs`. Nothing is unchecked (a store through
   `r[0]` meets `cs`'s own type at run time, and `d[r]` is refused there
   too), but the refusal should be a compile error at the element, as it
   is at a variable; and the literal's run-time type should then be
   `r`'s (it is the literal's own, array<array<C>>). A nested generic
   `array` component (`array<array>`) is a `dyn` at run time as it is
   statically (it matched any array there - fixed the same day; only a
   place's top-level generic `array` is a hole).
   **Done 2026-11-27 (check_flow_exact):** it was wider than the example
   - a shared container joined into ANY new container's element type
   unchecked, so `[cs, [2.5]]` with `cs` an array<int> was an
   array<array<float>> holding `cs`, read back as an int by the
   tree-walker and as a float by the VM (RULE 1 and RULE 2), and the
   same through an array sum, a dict literal's key or value, an
   `append`, an element store, a returned literal, a ternary or `??`.
   The flow check now recurses into what a NEW container is made of
   (literal elements, sum operands, a slice's base, a builtin's
   container elements) at every place: a variable, a result, an arm, an
   argument, a field, an element or dict store, an in-place builtin. Its
   run-time half (check_lit_elems_exact, in stamp_ctypes) checks each
   literal and array sum against the type it is MADE with, which covers
   a literal written in an expression (`print([cs, ds][0][0])`, a
   foreach's container). A container declared `dyn` takes anything.
   Tests: the `rule 1: a shared ...` -rt rows, tests/functional/148.
4. Rule 4 (storage follows the type), rule 5 (bare annotations are
   holes), rule 6 (`none` keys - already work, gets a test). Rules 4 and
   6 DONE 2026-11-21: a builtin's new container takes the storage of the
   type it has where it is made, not only as a variable's right-hand
   side (an inline `keys(d)` of a dict<int, int> was a general
   array<int>; tests/functional/134); `none` keys work in every engine
   (tests/functional/135), and a key that may be none into a dict whose
   keys may not is a compile-time NullabilityEx, as a value is (choice
   11) - it was a run-time fit failure for a literal `none`, and a
   TypeMismatchEx about the dict's type for a `dyn?`. Not done: making
   the invariant a CHECK (the JIT does not yet rely on it - #97's
   order); a container only `dyn` reached that ADOPTS a typed place's
   type keeps its storage (a general array of ints entering an
   array<int> parameter is then a general array<int>: converting it in
   place is the missing step, tests/int/106 builds one) - DONE
   2026-11-27 (flatten_general, called by adopt_detach: an array of
   non-none ints, floats or bools becomes flat as it takes the type, in
   place; a general array's kind can now change, so every walk a `dyn`
   name drives reads the kind per element, and sort's guard refuses the
   change as a modification - tests/functional/149, tests/int/138 and
   106). What still holds general storage under a typed array<int> is a
   CONSTANT's row (read-only, it takes no type), so the invariant is
   not yet a check; and rule 5.
   Rule 5 DONE 2026-11-21 (top level): a bare `array` / `dict`
   parameter is a template parameter (`template_param`, inferencer.cpp)
   - `f([1])` and `f(["s"])` are two instances where they were a join
   rule 1 refused, and a non-array argument is refused by the instance's
   kind check (or the bind, through `dyn`); a bare struct field is a
   SyntaxErrorEx (the 22 `-rt` rows, functional 83 and 130, tests/int/124
   and the show-decls session now write `array<dyn>` / `dict<dyn, dyn>`,
   the type a bare field had); a declaration was already filled from what
   it receives (`array a = [1, none]` is an array<int?>). A nullable bare
   parameter (`array? a`) is a template parameter too since rule 8.1.
   Not done: nested holes (`array<array>` is an array<array<dyn>>, as
   before). Tests:
   tests/functional/136, the `rule 5:` `-rt` rows. With no bare field
   left, a container or box field ALWAYS names its type: `.myv` v46
   refuses an image whose field breaks that (or whose scalar field has
   a type), and the passes that read a field's type lost their bare
   arms (reflection, the callee-set walk, the field fit check, the
   inferencer's field type).
5. Rule 8. 8.1 DONE 2026-11-21: an un-annotated `opt` parameter (and a
   nullable bare `array?` / `dict?`) is a template parameter, keyed by the
   argument's type without its `?`; a `none` or omitted argument is an
   instance of its own (provisional choice 16, section 8), and a `none`
   given to an `opt` parameter adds nothing to its type, so that
   instance's parameter is a `dyn?`. A late fold now refuses a call that
   still reaches a template base (a RULE 2 divergence 8.1 exposed: the
   parse's copy instantiated `T2[2](3)` while the program ran the base).
   Tests: tests/functional/137, the `rule 8.1:` `-rt` rows. 8.2 (closure
   and lambda-value instances) not started.

## 10. Rule 8.2 - the design, approved 2026-11-22, BUILT 2026-11-22

Stopped at the design on purpose: 8.2 changes the runtime call model
and the `.myv` format, so the maintainer reviewed it before any code
(his decisions are at the end of this section).

**What it is.** A capturing closure, and a lambda used as a value, get
typed instances the way a named template does - but the program still
makes ONE closure object per evaluation of the lambda (one capture
vector: captures are shared, mutable state, so two objects would split
a counter). The instances are BODIES, not objects. Script-visible: a
closure's parameters stop joining across its call sites (today
`f(1); f(2.5)` makes `x` a float in both calls and `f(1); f("s")` is
refused); each site gets its own instance, as a named template's does,
so README's *Static type inference* changes with the code.

**Inference.** Treat such a lambda like a template whose instances are
keyed per call site, using the sites the callee-set analysis attributes
to it (`callee_set(call->what)` naming exactly this lambda, `!top`): a
site that names it alone gets the instance for its argument types
(template_arg_key, so `opt` parameters key as in 8.1); every other site
- several candidates, or ⊤ - calls the untyped BASE body. The base is
kept whenever such a site exists (the #149 closure keeps it today for a
value-used template). No uniformity rule: each site picks its own.

**Runtime.**
- `FuncDescriptor` gains `instances` (descriptors of the instance
  bodies, each with its own chunk); an instance shares the base's
  capture layout - it is a clone of the same lambda, so capture slot `k`
  means the same captured variable in every body.
- A call site that picked instance `k` compiles to a call that names
  `k`: a new operand on the closure call ops (CallValueV and its
  frameless / record-less tiers) - the callee value is still the
  FuncObject (its capture vector), the body is `desc->instances[k]`. A
  guard that the value really is this lambda's object (its base
  descriptor) protects the choice where the analysis can be wrong only
  for a loaded image: the #97 GuardCalleeV identity check, already
  emitted for the value splices, is that guard; its miss runs the base.
- The tree-walker's `do_func_call` gets the instance's decl and the
  object's captures.
- The bytecode inliner may paste an instance body exactly as it pastes
  a value-call's callee today (behind the same guard).

**Format.** `.myv` stores each descriptor's instance list (descriptor
table indices) - a version bump, docs/myv-format.txt in the same
commit, `tests/myv_doc_check`; the loader checks each instance's
capture count equals its base's (a mutated image must not make an
instance read past the capture vector).

**Decided (maintainer, 2026-11-22).**
1. A site that may reach two or more lambdas calls each one's untyped
   BASE. (A guard chain over per-candidate instances would also refuse
   valid programs: every candidate would have to type-check with the
   site's arguments, including one that never runs there - `h("x")`
   with `h` either `up(s) => s + "!"` or `neg(n) => -n`.)
2. The instance cap is the templates' one, 64 per lambda
   (`--max-instances`).
3. The REPL: a closure made in one input and called in a later one runs
   its base, as a value-used template does today (`typestr(x)` in its
   body says "dyn" there where a script says "int"). A later input
   adding an instance to an earlier input's closure is left for later.

**Order:** inference + `-dti`/`:show` visibility first (no
runtime change: instances unused), then the call operand and the
tree-walker, then the VM tiers one at a time with the five-mode
differential, then the format.

**Built (2026-11-22), and where it departs from the design above** (the
implementation record is CLAUDE.md, *Closure templates*):
- the runtime does not add an operand to the call ops: a new op,
  InstanceSelectV, turns the callee into an INSTANCE VIEW (the instance's
  descriptor, the base object's capture slots) before an unchanged call,
  so no call tier changed. A view is a small allocation per call; the
  common shape - one instance serving every site - needs none (the
  COLLAPSE: the closure is made with the instance's descriptor);
- the base is an ordinary checked function whose template parameters stay
  unconstrained (no site contributes), not one "typed dyn" - typing them
  `dyn` made every value derived from them `dyn` during inference, which
  the mandatory-dyn rule refused (`var a = f(3)` at a site that runs the
  base);
- PROVISIONAL (for the maintainer): a lambda a higher-order builtin is
  handed (map, filter, sort, rev_sort, sum, make_array, make_dict, find)
  keeps the old join with its other call sites - the builtin calls the
  closure object, whose body is the base, and types its parameters from
  the container;
- the base may not store an untyped value into a variable it does not own:
  that store is checked at run time against the variable's type
  (`base_shared_dyn`), which needed a pre-existing bug fixed first - a
  captured variable's declared type did not reach the stores in the
  closure's body.
