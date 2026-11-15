# A container's type belongs to the container (Q2, 2026-11-16)

**Status: the rule is the maintainer's (2026-11-16). One part is open:
how a container is checked where it ENTERS a typed place (section 3).**

## 1. The rule

A variable that is not declared `dyn` is statically typed. A `dyn`
name is a union: at any moment it holds one value of one type, and
that value keeps its own type. So a typed container reached through a
`dyn` name is still typed, and a store that does not fit it fails:

    var m = [[1], [2]];   var dyn f = m;   f[0] = ["s"];
    var s = ["a", "b"];   var dyn g = s;   g[0] = 5;
    var dd = {"k": 1};    var dyn h = dd;  h["k"] = "str";

Today (ran on ea7bbd87) all three succeed: `m` is an
`array<array<int>>` holding `["s"]`, `s` an `array<str>` holding 5, `dd`
a `dict<str,int>` holding "str". An array whose STORAGE encodes its
element type (int, float, bool, a flat struct) already refuses such a
store with TypeErrorEx; general storage and every dict accept anything.
After this change all three raise TypeErrorEx, and the README's
documented gap ("a container reached through a `dyn` name is not a
typed place", *A `dyn` value entering a typed place*) goes away.

The maintainer, on `var dyn d = a`: it "should not make the array 'a'
dynamic ... at a specific point in time, 'd' points to a typed array,
so 'd[1] = Q(...)' must fail, not because 'd' is not dyn, but because
the type inside the discriminated union, the array, is not dynamic."
And a container that is dynamic should be made so where it is created:
"the proper behavior would have been to make 'a' point to a dynamic
array in the first place".

## 2. Where a container gets its type

Its static type where it is created, fixed for its lifetime:

- a literal: the type of the place it is written into when that place
  has one (a declared variable, element, field, parameter - the same
  destinations whose type already picks the literal's storage,
  `set_array_repr_hint` / `literal_into`), else its own static type;
- a literal written where `dyn` is declared (`var dyn d = [1, 2]`, an
  `array<dyn>` element, a `dyn` field) is dynamic: `array<dyn>`;
- an empty `[]` / `{}` with no known type is `array<dyn>` /
  `dict<dyn, dyn>` (question 9 already reads its element as unfilled);
- a builtin's result: the call's static type (`keys(d)` of a
  `dict<str,int>` is `array<str>`; `map`, `filter`, `range`,
  `make_array`, `split`, `sort`'s copy, `clone`, `deepclone`, a slice -
  which shares its storage and so its type - and `a + b`);
- `dynarray(a)`: `array<dyn>`, as today;
- a value read from a `.myv` image: the type stored with it.

## 3. OPEN: a container entering a typed place

Today a container entering a typed place is checked by its CONTENTS
at that moment (#49, `value_fits` walking a general array or a dict).
That cannot make the place safe, because the container stays reachable
through the name it came from. With no `dyn` name involved (ran on
ea7bbd87, all engines):

    array<dyn> q = [1, 2];
    array<int> a = q;      # accepted: every element is an int now
    q[0] = "s";            # accepted: q's array is dynamic
    print(a);              # array<int> holds ["s", 2]
    print(a[0] + 1);       # TypeErrorEx here

- **A. Check the container's own type (invariance).** A container
  enters a typed place only if its type is the place's type.
  `array<int> a = q` is a compile error (`q` says `array<dyn>`); a
  dynamic array reaching an `array<int>` parameter through a `dyn`
  value is a TypeErrorEx even when every element is an int. Sound;
  what C# does (`List<object>` is not a `List<int>`).
- **B. Keep the contents check.** Closes the stores through a `dyn`
  name (section 1) and leaves the example above as it is.

Measured cost of A (ea7bbd87, an instrumented build reporting every
container the contents check accepted, over tests/functional,
tests/int, bench and samples): 9 programs, all of them tests, most of
them #49's own tests of the contents check; no bench or sample.

## 4. Design

- **Representation.** One pointer per container: `SharedObject` gets
  the array's element type, `DictObject` its key and value types, as a
  `const TypeAnnot *` into a process-lifetime INTERN table
  (`annot_intern`), so two equal types are one pointer and comparing
  types is a pointer compare. Null means `dyn`. Storage stays an
  implementation detail chosen as today; a flat array that would
  PROMOTE to general storage on a misfit refuses it instead unless its
  type allows the value.
- **Creation.** The compiler computes each creation site's type and
  pools it: codegen beside the literal pool and `fit_specs`, a builtin
  call through its `ArgLocs` (beside `arr_hint`), a baked `LiteralObj`
  inside its value. Every engine builds through the same helpers, so a
  site that is not stamped is found by a test, not by a user.
- **Checked stores.** A store the compiler could not check - its
  container's static type is `dyn` or has a `dyn` on the path to it -
  is marked by the compiler and checks the value against the
  container's type (`value_fits` and its message). Statically checked
  stores pay nothing. Forms: an element store, a dict store, a member
  store into a dict through `dyn`, the chain stores, the in-place
  builtins (`append`, `push`, `insert`) and `+=` on an array. The VM
  carries the mark in the op; the JIT's inline element-store tiers send
  a marked store to their helper.
- **Entry.** Per section 3.
- **`.myv`.** An array or dict value record carries its type (the
  annotation encoding `fit_specs` already uses); format version bump,
  docs/myv-format.txt and `tests/myv_doc_check` in the same commit.

## 5. Increments

1. The intern table and the carried type; every creation site stamped;
   `typestr()` of a `dyn` value reports the container's own type, which
   is also the test that a site was stamped.
2. Checked stores through `dyn`, every engine, and the in-place
   builtins; the README gap removed.
3. The entry check (section 3).
4. `.myv`.

Tests: a functional program with every store form and every container
shape, each refused and each fitting (old value kept); `-rt` cases per
engine; a `driver_checks` image round trip.
