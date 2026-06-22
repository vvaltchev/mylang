# MyLang

![Linux](https://github.com/vvaltchev/mylang/workflows/Linux/badge.svg)
![Windows](https://github.com/vvaltchev/mylang/workflows/Windows/badge.svg)
![MacOSX](https://github.com/vvaltchev/mylang/workflows/MacOSX/badge.svg)
![Coverage](https://github.com/vvaltchev/mylang/workflows/Coverage/badge.svg)
[![codecov](https://codecov.io/gh/vvaltchev/mylang/branch/master/graph/badge.svg?token=B3L5Z6T6PR)](https://codecov.io/gh/vvaltchev/mylang)

## What is MyLang?

MyLang is a simple educational programming language inspired by `Python`,
`JavaScript`, and `C`, written as a personal challenge, in a short time,
mostly to have fun writing a *recursive descent parser* and explore the
world of interpreters. Don't expect a full-blown scripting language with
libraries and frameworks ready for production use. However, `MyLang` has
a minimal set of builtins and, it *could* be used for practical purposes
as well.

## Contents

  * [Maintainance](#maintainance)
    * [Building MyLang](#building-mylang)
    * [Testing MyLang](#testing-mylang)
  * [Syntax](#syntax)
    * [Core concepts](#core-concepts)
    * [Declaring variables](#declaring-variables)
      - [Explicit types](#explicit-types)
    * [Declaring constants](#declaring-constants)
      - [Automatic const promotion](#automatic-const-promotion)
    * [Type system](#type-system)
    * [Conditional statements](#conditional-statements)
      - [Const evaluation](#const-evaluation-of-conditional-statements)
    * [Classic loop statements](#classic-loop-statements)
    * [The foreach loop](#the-foreach-loop)
      - [The "indexed" keyword](#extra-features-the-indexed-keyword)
      - [Array expansion](#extra-features-array-expansion)
    * [Functions and lambdas](#functions-and-lambdas)
      - [Const parameters](#const-parameters)
      - [Typed parameters](#typed-parameters)
      - [Named arguments](#named-arguments)
      - [Lambda captures](#lambda-captures)
      - [Calling functions during const-evaluation](#calling-functions-during-const-evaluation)
      - [Pure functions](#pure-functions)
      - [Automatic pure promotion](#automatic-pure-promotion)
    * [Structs](#structs)
    * [Exceptions](#exceptions)
      - [Custom exceptions](#custom-exceptions)
      - [Re-throwing an exception](#re-throwing-an-exception)
      - [The finally clause](#the-finally-clause)
  * [Builtins](#builtins)
    * [Const builtins](#const-builtins)
      - [General builtins](#general-builtins)
      - [Array builtins](#array-builtins)
      - [Dictionary builtins](#dictionary-builtins)
      - [String builtins](#string-builtins)
      - [Generic-container builtins](#generic-container-builtins)
      - [Numeric builtins](#numeric-builtins)
      - [Numeric constants](#numeric-constants)
    * [Non-const builtins](#non-const-builtins)
      - [Misc builtins](#non-const-misc-builtins)
      - [Array builtins](#non-const-array-builtins)
      - [Generic-container builtins](#non-const-generic-container-builtins)
      - [Numeric builtins](#non-const-numeric-builtins)
      - [I/O builtins](#non-const-io-builtins)


## Maintainance

### Building MyLang

MyLang is written in *portable* C++17: at the moment, the project has no
dependencies other than the standard C++ library. To build it, if you have
`GNU make` installed, just run:

```
$ make -j
```

Otherwise, just pass all the .cpp files to your compiler and add the `src/`
directory to the include search path. One of the nicest things about *not*
having dependecies is that there's no need for a *build system* for one-time
builds.

#### Out-of-tree builds

Just pass the `BUILD_DIR` option to `make`:

```
$ make -j BUILD_DIR=other_build_directory
```

### Testing MyLang

If you want to run MyLang's tests as well, you need to just compile with TESTS=1
and disable the optimizations with OPT=0, for a better debugging experience:

```
$ make -j TESTS=1 OPT=0
```

Then, run all the tests with:

```
$ ./build/mylang -rt
```

It's worth noticing that, while test frameworks like [GoogleTest] and [Boost.Test]
are infinitely much more powerful and flexible than the trivial test engine we have
in `src/tests.cpp`, they are *external* dependencies. The less dependencies, the
better, right? :-)

[GoogleTest]: https://github.com/google/googletest
[Boost.Test]: https://www.boost.org/doc/libs/1_75_0/libs/test/doc/html/index.html

## Syntax

The shortest way to describe `MyLang` is: *a C-looking dynamic python-ish
language*. Probably, the fastest way to learn this language is to check
out the scripts in the `samples/` directory while taking a look at the short
documentation below.

### Core concepts

`MyLang` is a dynamic duck-typing language, like `Python`. If you know `Python`
and you're willing to use `{ }` braces, you'll be automatically able to use it.
No surprises. Strings are immutable like in `Python`, arrays can be defined using
`[ ]` like in `Python`, and dictionaries can be defined using `{ }`, as well.
The language also supports array-slices using the same `[start:end]` syntax used
by `Python`.

Said that, `MyLang` differs from `Python` and other scripting languages in several
aspects:

  - There's support for *parse-time* constants declared using `const`.

  - All variables *must be* declared using `var`.

  - Variables have a scope like in `C`. Shadowing is supported when a variable
    is explicitly re-declared using `var`, in a nested block.

  - All expression statements must end with `;` like in `C`, `C++`, and `Java`.

  - `true` and `false` are the two values of a real `bool` type (so
    `type(true) == "bool"`, distinct from `int`). `bool` sits at the bottom of
    the numeric promotion chain `bool <= int <= float`: a bool used in
    arithmetic promotes to the integers `0`/`1` (`true + 1 == 2`,
    `sum([true, true]) == 2`), and `true == 1`, `false == 0` compare equal (and
    hash equal, so `true` and `1` are the same dict key). Comparisons
    (`== != < <= > >=`), the logical operators (`&& || !`), and a `bool`
    literal/variable all have type `bool`. The **truthiness** rule is unchanged:
    `0`, `none`, and an empty string/array/dict are false, everything else is
    true — so any value can still be used as a condition. Arrays of `bool` get a
    compact one-byte-per-element flat representation (`array_storage()` reports
    `"bools"`), like flat `int`/`float` arrays.

  - The assignment operator `=` can be used like in `C`, inside expressions, but
    there's no such thing as the comma operator, because of the array-expansion
    feature.

  - MyLang supports both the classic `for` loop and an explicit `foreach` loop.

  - MyLang supports user-defined **struct** types (see *Structs* below).
    Dictionaries also support a nice syntactic sugar: in addition to the main
    syntax `d["key"]`, for string keys the syntax `d.key` is supported as well
    (`.` is struct-field access on a struct, dict-key access on a dict).

### Declaring variables

Variables are always declared with `var` and live in the scope they've been declared
(while being visible in nested scopes). For example:

```C#
# Variable declared in the global scope
var a = 42;

{
    var b = 12;
    # Here we can see both `a` and `b`
    print(a, b);
}

# But here we cannot see `b`.
```

It's possible to declare multiple variables using the following familiar syntax:

```C#
var a,b,c;
```

But there's a caveat, probably the only "surprising" feature of `MyLang`:
initializing variables doesn't work like in C. Consider the following
statement:

```C#
var a,b,c = 42;
```

In this case, instead of just declaring `a` and `b` and initializing to `c` to 42,
we're initializing *all* the three variables to the value 42. To initialize each
variable to a different value, use the array-expansion syntax:

```C#
var a,b,c = [1,2,3];
```

#### Explicit types

A declaration may use an explicit primitive type instead of `var`. The type
names are `bool`, `int`, `float`, `str`, `array`, and `dict`:

```C#
int   x = 32;
float ratio = 1.5;
str   name = "Neo";
bool  done = false;
array nums = [1, 2, 3];     # generic array; the element type is still inferred
dict  conf = {"n": 1};
```

It is the same declaration as `var`, plus a **compile-time type constraint**:
the value (and any later assignment) must be assignable to the declared type, or
you get a compile error.

  * **Scalars** (`bool`/`int`/`float`/`str`) pin the variable's type. Assigning
    an incompatible value is an error (`int x = "hi"`, `int x = 3.5`, or a later
    `x = 2.5`). The numeric chain still applies: `float f = 3;` is fine and
    **coerces** the value to `3.0` (so `f` really holds a float, including after
    `f = 5;`), and `int x = true;` stores `1`.
  * **`array`/`dict`** are *generic*: they only require the value to be an array
    / dict, while its element/key/value types are inferred as usual — so
    `array nums = [1,2,3]` is still a fast flat `array<int>` (parameterized
    `array<T>` / `dict<K,V>` syntax is not available yet).
  * **No initializer** gives the type's zero value: `int x;` → `0`, `float x;` →
    `0.0`, `bool x;` → `false`, `str x;` → `""`, `array x;` → `[]`,
    `dict x;` → `{}`. (An `opt`-qualified one defaults to `none`.)
  * It works in a `for` initializer too: `for (int i = 0; i < n; i += 1) ...`.

The type keywords are still ordinary identifiers everywhere else — `int(x)`,
`array(n)`, `map(str, xs)` keep working — they are read as a type only at the
start of a declaration (when immediately followed by the variable name).

##### Nullable types: the `?` suffix

Append **`?`** to the type to make it **nullable** (an `Optional<T>`, as in
Kotlin/Swift) — it may also hold `none`/`null`. This is the canonical short form
of the `opt` keyword and works on every declaration kind, including `var?`
(inferred) and `dyn?` (dynamic):

```C#
int?   x;            # nullable int, defaults to none
str?   s = null;     # `null` is just `none`
var?   maybe;        # inferred type, nullable
dyn?   anything;     # dynamic and nullable  (== opt dyn)
array? items;        # nullable array
```

`int? x` is exactly `opt int x`; `var? x` is `var opt x`; `dyn? x` is
`opt dyn x`. A `?`-typed declaration with no initializer defaults to `none`
(rather than the zero value), since it is explicitly nullable. `dyn` may also be
used directly as a declaration keyword (`dyn z = 5;`), not only as a modifier
after `var`.

Parameters can be typed the same way (see *Functions* below), and additionally
get two terse param-only short forms:

```C#
func dist(float x, float y) => sqrt(x*x + y*y);   # typed params
func f(x, y?, ~z?) => ...    # y optional; z optional and dynamic (~ = dyn)
```

### Declaring constants

Constants are declared in a similar way as variables *but* they cannot be shadowed
in nested scopes. For example:

```C#
const c = 42;

{
    # That's not allowed
    const c = 1;

    # That's not allowed as well
    var c = 99;
}
```

In `MyLang` constants are evaluated at *parse-time*, in a similar fashion to `C++`'s
*constexpr* declarations (but there we talk about *compile time*). While initializing
a `const`, any kind of literal can be used in addition to the whole set of *const*
builtins. For example:

```C#
const val = sum([1,2,3]);
const x = "hello" + " world" + " " + join(["a","b","c"], ",");
```

To understand how exactly a constant has been evaluated, run the interpreter
with the `-s` option, to dump the *abstract syntax tree* before running the script.
For the example above:

```
$ cat > t
    const val = sum([1,2,3]);
    const x = "hello" + " world" + " " + join(["a","b","c"], ",");
$ ./build/mylang t
$ ./build/mylang -s t
Syntax tree
--------------------------
Block(
)
--------------------------
```

Surprised? Well, constants other than arrays and dictionaries are not even
instantiated as variables. They just don't exist at *runtime*. Let's add a
statement using `x`:

```
$ cat >> t
    print(x);
$ cat t
    const val = sum([1,2,3]);
    const x = "hello" + " world" + " " + join(["a","b","c"], ",");
    print(x);
$ ./build/mylang -s t
Syntax tree
--------------------------
Block(
  CallExpr(
    Id("print")
    ExprList(
      "hello world a,b,c"
    )
  )
)
--------------------------
hello world a,b,c
```

Now, everything should make sense. Almost the same thing happens with arrays
and dictionaries with the exception that the latter ones exist at runtime as
well, in order to avoid having potentially huge literals everywhere.
Consider the following example:

```
$ ./build/mylang -s -e 'const ar=range(4); const s=ar[2:]; print(ar, s, s[0]);'
Syntax tree
--------------------------
Block(
  ConstDecl(
    Id("ar")
    Op '='
    Obj([0, 1, 2, 3])
  )
  ConstDecl(
    Id("s")
    Op '='
    Obj([2, 3])
  )
  CallExpr(
    Id("print")
    ExprList(
      Id("ar")
      Id("s")
      Int(2)
    )
  )
)
--------------------------
[0, 1, 2, 3] [2, 3] 2
```

As you can see, the *slice* operation `ar[2:]` has been evaluated at
*parse-time* while initializing the constant `s`, but both arrays still exist
at runtime. A
const array/dict value is baked into a single `Obj(...)` node holding the value
(not one literal per element), so even a large result stays one node in the
tree. By contrast, a *scalar* result — like the subscript `s[0]` — folds all the
way to a plain literal (here `Int(2)`). That's a good trade-off for performance:
small values like integers, floats, and strings become literals during the
*const evaluation*, while arrays and dictionaries (potentially big) are kept as
read-only symbols at runtime, but still allowing some operations on them (like
`[index]` and `len(arr)`) to be const-evaluated.

A `const` array or dictionary is **deeply read-only**: there is no shallow
const, so a const container and every element nested inside it are immutable.
The read-only-ness is a property of the *value*, not just the name — so it holds
even when the value is aliased through a non-const binding, in particular a
function parameter:

```C#
func g(p) {
    p = [9, 9];   # OK: rebinding the parameter is allowed
    p[0] = 1;     # OK: p now refers to g's own fresh array
    return p;
}

const y = [1, 2, 3];
print(g(y));      # [1, 9]
print(y);         # [1, 2, 3] — untouched
```

Trying to *mutate* the value of a const (rather than rebind a name to a new
value) is an error: `p[0] = x`, `p.k = x` and inserting a missing dict key fail
with `NotLValueEx`, while `p += [...]`, `append`/`insert`/`pop`/`erase` fail
with `CannotChangeConstEx`. `sort()` of a const returns a sorted *copy* and
leaves the original alone.

Read-only-ness **propagates**: a value *derived* from a const — a slice
`y[1:3]`, a sub-object, a function result that returns part of a const — is
itself read-only, and binding it to a `var` keeps it read-only. `var` only makes
the *name* rebindable; it does not make a const value mutable:

```C#
const y = [1, 2, 3];
var s = y[1:3];   # s is a read-only slice of a const
s[0] = 9;         # error: NotLValueEx
s = [9, 9];       # OK: rebinding the name s is allowed
```

To get a mutable copy you must ask for one explicitly: `clone(x)` makes a
**shallow** mutable copy (only the top level is copied; nested objects are
shared, so nested objects of a const stay read-only), while `deepclone(x)` makes
a **fully mutable deep copy** that you can change at any depth. A *fresh*
literal is not derived from a const, so `var a = [1, 2, 3]` is mutable as usual
(though an element that is itself a const stays read-only — `var a = [y]` with
`y` const keeps `a[0]` read-only).

#### De-duplication of const expressions

Identical constant array/dictionary expressions are **evaluated only once**. The
first time the parser bakes such a value it remembers it (keyed by the
expression's shape, with identifiers resolved to the exact constant they refer
to, so shadowing never aliases); a later identical const expression reuses that
same read-only value instead of recomputing and re-allocating it. This is
standard *common-subexpression elimination*, done at parse time. Because the
shared value is deeply read-only, aliasing it is always safe.

```C#
const big = range(1000);
const a = big[0:500];
const b = big[0:500];     # same expression
assert(intptr(a) == intptr(b));   # a and b are the *same* buffer

const c = big[0:400];     # a different expression
assert(intptr(a) != intptr(c));
```

It works for any const array/dict result whose subexpressions are all
de-duplicable — slices, subscripts, concatenations of const identifiers, and
`pure`/const-builtin calls such as `sort(base)` — and it is purely an
optimization: the program behaves exactly as if each occurrence were computed
independently (only `intptr()` reveals the sharing). The payoff is **compile
time** (a heavy const expression written *N* times is computed once, not *N*
times) and **memory** (*N* const tables derived the same way share one buffer
instead of *N*). The de-duplication is scoped to the lexical block, so values
keyed against a block's constants are released when that block's parsing ends.
A read-only value is required, so a mutable `var a = [1, 2, 3]` is never shared
with another — each `var` gets its own writable copy. (See
`bench/52_cse_dedup` and `bench/README.md` for the measured effect.)

#### Automatic const promotion

You don't have to write `const` to get most of these benefits. A variable
declared with `var` that is *written exactly once* (its declaration) with a
constant scalar initializer — and is never reassigned, captured, `undef()`-ed,
or used in a position that needs an lvalue — is automatically promoted to a
constant. Its uses are then folded just like a `const` and its decl disappears.
This *auto-const* pass runs after parsing, so it also handles values *derived*
from other auto-consts:

```C#
var A = 7;             # write-once -> auto-const
var B = A * 3 + 1;     # = 22, folded at "compile" time -> auto-const too
for (var i = 0; i < n; i += 1) {
    var k = A * B - 5; # constant; folded to a single literal, not recomputed
    s += k + i;
}
```

Auto-const also performs *dead-code elimination*: an `if`/`while` whose
condition folds to a constant has its dead branch dropped (and a `while (false)`
removed). Unlike literal/`const` expressions (which the parser folds eagerly,
everywhere), auto-const only analyzes code it proves *reachable*: a branch it
proves dead is eliminated, not checked.

Because folding evaluates the expression at "compile" time, a fully-constant
expression that *always* fails is rejected **before the script runs**, exactly
like a `const`:

```
$ ./build/mylang -e 'var a = 6; var b = 0; print(a / b);'
DivisionByZeroEx: Division by zero at line 1, col 30:32

    var a = 6; var b = 0; print(a / b);
                                ^^^^^
```

This is intentional: a value we can fully compute at compile time that can never
succeed is a program that can never run correctly, so it is a build error, not a
runtime exception. `try`/`catch` is for *runtime* exceptions and does **not**
catch these. If you want such an expression to stay a (catchable) runtime error
— for tests, or to opt out of folding — wrap the runtime-varying part in
[`runtime()`](#runtimeexpr): `a / runtime(b)` throws `DivisionByZeroEx` at
runtime instead.

### Type system

`MyLang` supports, at the moment, only the following (builtin) types:

  * **None**
    The type of `none` (also spelled **`null`** — the two are exact aliases),
    the equivalent of Python's `None`. Variables just declared without having a
    value assigned to them have value `none` (e.g. `var x;`). The same applies to
    functions that don't have a return value. Also, it's used as a special value
    by builtins like `find()`, in case of failure.

  * **Boolean**
    The type `bool`, with exactly two values: `true` and `false`. It is the
    bottom of the numeric chain `bool <= int <= float`, so a bool promotes to
    the integers `0`/`1` in arithmetic (`true + 1 == 2`) and `true == 1`,
    `false == 0` (also hashing equal — same dict key). Comparisons and the
    logical operators (`&& || !`) produce `bool`. Truthiness is unchanged: `0`,
    `none`, and empty containers/strings are false, everything else true.

  * **Integer**
    A *signed* pointer-size integer (e.g. `3`).

  * **Float**
    A floating-point number (e.g. `1.23`). Internally, it's a C `double`
    (64-bit IEEE 754), exactly like Python's `float`.

  * **String**
    A string like "hello". Strings are immutable and support slices (e.g.
    `s[3:5]` or `s[3:]` or `s[-2:]`, having the same meaning as in `Python`).

  * **Array**
    A mutable type for arrays and tuples (e.g. `[1,2,3]`). It can contain items
    of different type and it supports writable slices. Array slices behave like
    copies while, under the hood, they use copy-on-write techniques.

  * **Dictionary**
    Dictionaries are hash-maps defined using `Python`'s syntax: `{"a": 3}`.
    Elements are accessed with `d["key-string"]` or `d[23]`, looked-up with
    `get()`/`get!()`/`find()`, and deleted with `erase()`. Only strings, ints,
    and floats can be keys. **Perks**: identifier-like string-keys can be
    accessed with the "member of" syntax: `d.key`. A read of a *missing* key
    (`d[k]`/`d.key`) raises `KeyNotFoundEx` — non-`opt` access (a value or an
    exception, never `none`); use `get()` for a nullable lookup, or
    `dict(default)` for a default value. A write (`d[k] = v`) inserts the key.

  * **Function**
    Both standalone functions and lambdas have the same object type and can be passed
    around like any other object (see below). But, only lambdas can have a capture list.
    Regular functions cannot be executed during const-evaluation, while `pure` functions
    can. Pure functions can only see consts and their arguments.

  * **Exception**
    The only type of objects that can be thrown. To create them, use the `exception()`
    builtin or its shortcut `ex()`.

### Static type inference

Even though MyLang has no type annotations, the whole program is type-checked
**at compile time**, before it runs. The compiler infers a single fixed type for
every variable, parameter, and function return by analyzing the entire source
(it's one file, so nothing is hidden), and reports a **compile-time error** for
any type violation. These errors are *not* catchable with `try/catch` — like a
syntax error, they fail the build. Type checking can be disabled with the `-nti`
flag (e.g. while migrating old code).

What this catches before running:

```C#
var x = 5;
x = "hi";              # error: x is int, cannot be assigned a str

func add(a, b) => a + b;
add(1, 2, 3);          # error: add expects 2 arguments, got 3

func sq(n) => n * n;
sq(4);                 # ok: n inferred as int from this call
sq("z");               # error: n would have to be both int and str
```

Key rules:

  * **A plain `var`/`const` must infer a concrete type.** If the only type the
    compiler can give a plain declaration is *dynamic* (`dyn`), that is a
    compile error: you must declare it `dyn` (e.g. `var dyn x = ...`) to opt
    into dynamic typing explicitly. There is no implicit `dyn` — a value that is
    genuinely polymorphic (a builtin used as a value, a `runtime(...)` result,
    an exception payload, a variable that holds different unrelated types) must
    say so. (A *container* with mixed elements is `array<dyn>` / `dict<_,dyn>`
    and is still accepted under a plain `var` — only a bare top-level `dyn`
    requires the keyword.)
  * A variable's type is the **join** of everything assigned to it. Assigning an
    incompatible type on any path is an error. `int` automatically widens to
    `float` (so `var x = 1; x = 2.5;` is fine, and `x` becomes a `float`).
  * A **function's parameter and return types are inferred from how it is called
    and what it returns**, across the whole program (recursion and forward
    references included). A function called with conflicting argument types for
    the same parameter is an error.
  * Arrays and dicts are typed by their contents (`[1,2,3]` is an array of int).
    A genuinely mixed container is allowed — its element type just becomes `dyn`
    (see below), so you don't get the speed/safety benefits but it still works.
  * **Null safety.** A plain value is *non-nullable*: passing `none` where a
    non-`none` value is required — an arithmetic operand, a subscript base — is
    a compile error. A local that might be `none` (e.g. `var x;`, or assigned
    only on some branches) is treated as nullable, so using it directly where a
    value is required is rejected — initialize it (`var x = 0;`) or declare it
    `opt` (or `opt dyn`). Note `dyn` alone is **non-null** (see below).
  * **A parameter that can receive `none` must be declared `opt`.** If any call
    (transitively, across the whole program) could pass `none` to a parameter
    that is not `opt`, it is a compile error *at the param's declaration* asking
    you to mark it `opt` (or `opt dyn` for a dynamically-typed one). So a
    non-`opt` parameter is
    *guaranteed* never to be `none` — the body can use it without a check. This
    is the nullability analogue of the mandatory-`dyn` rule.
  * **A dict read is non-`opt` — it throws on a missing key.** `d[k]` / `d.k`
    return the value (a value or an exception, never `none`), so they are usable
    without a check; a missing key raises `KeyNotFoundEx` at runtime. For a
    nullable or fail-fast lookup use `get()` / `get!()`, and for the "absent ==
    a default" pattern use a default dict (`dict(default_value)`) — see the dict
    builtins. A write (`d[k] = v`) still inserts a new key as usual.
  * `==` and `!=` work between any two values (they never error, returning a
    `bool`); ordering operators `< <= > >=` need two numbers or two strings and
    also return a `bool`. The logical operators `&& || !` return a `bool` too.
    A comparison/logical result therefore infers as `bool` (e.g. `var ok = a <
    b;` makes `ok` a `bool`), and an array of such results is `array<bool>`.

#### The `opt` and `dyn` modifiers

Two keywords opt out of the strict defaults, usable on a parameter or a
`var`/`const` declaration:

```C#
func greet(opt name) {           # `name` may be none
    if (name == none) return "hi";
    return "hi " + name;
}

func generic(dyn x) => x;        # `x` is dynamically typed (anything goes)

var dyn slot = 5;                # `slot` behaves like a classic dynamic var
slot = "now a string";           # ok, because it's dyn
```

  * **`opt`** — *nullable*: the value may be `none`. A non-`opt` parameter is
    guaranteed never to receive `none`, which is what makes the null-safety
    check above possible. Like `dyn`, `opt` is **required**, not optional, on a
    parameter that can actually receive `none` from some call path — otherwise
    you get a compile error at the param's declaration telling you to add it.
    The **`?` type suffix** is the canonical short form of `opt` (`int? x` ≡
    `opt int x`, `var? x` ≡ `var opt x`, `dyn? x` ≡ `opt dyn x`) — see *Nullable
    types* above.

    **Trailing `opt` parameters are also optional at the call site** (the other
    sense of "optional"): the caller may omit them, and each omitted one binds
    to `none`. So `func foo(x, opt y, opt z)` can be called as `foo(1)`,
    `foo(1, 2)`, or `foo(1, 2, 3)` — the legal argument count is a range. The
    minimum is "up to and including the last non-`opt` parameter", so a non-opt
    param *after* an opt one simply can't be skipped: `func f(x, opt y, z)`
    still requires all three. Passing fewer than the minimum (or more than the
    total) is a compile-time arity error.
  * **`dyn`** — *dynamically typed*: the value may hold any **type** and may
    change type; *type* operations on it are checked at runtime, not at compile
    time. Use it for variant values — a variable or an array element that
    genuinely holds different types (`var dyn a = [1, "two", 3.0]`), without the
    manual tagged-union bookkeeping a fixed-type language needs. `dyn` is
    **required**, not optional, wherever a plain declaration would otherwise
    infer `dyn` (see the first key rule above) — so `var x = someBuiltin;` or
    `var x = runtime(v);` must be written `var dyn x = ...`.

    **Nullability is orthogonal to `dyn`.** A bare `dyn` is *non-null* (proven
    never `none`, so usable without a check); `none` is allowed only with
    `opt dyn`. The four combinations: `x` (typed, non-null), `opt x` (typed,
    nullable), `dyn x` (dynamic, non-null), `opt dyn x` (dynamic, nullable). So
    `dyn` opts out of *type* checking but **not** null-safety — passing `none`
    to a non-`opt` `dyn` parameter is still a compile error asking for
    `opt dyn`.

#### Null-checks are understood (narrowing)

After you check a nullable value against `none`, it is treated as non-`none` in
the guarded code, so these all type-check:

```C#
func f(opt x) {
    if (x == none) return 0;     # guard clause
    return x + 1;                # x is known non-none here
}
func g(opt x) {
    if (x != none) return x * 2; # x is known non-none in this branch
    return -1;
}
```

#### Speed

Type inference is not only about safety: knowing a variable is an `int` or a
`float` lets the interpreter evaluate arithmetic, comparisons and loop
conditions over it **without** the per-operation type dispatch and value-boxing
a dynamic language normally pays. On numeric code this is a large win — e.g.
`bench/my/44_primes_sqrt.my` runs ~2.8x faster with inference on than off, and
float-heavy loops (`bench/my/54_mandelbrot.my`, `55_float_sum.my`) run **faster
than CPython**. Code typed `dyn` keeps the original dynamic behavior and speed.

### Conditional statements

Conditional statements work exactly like in `C`. The syntax is:

```C#
if (conditionExpr) {
    # Then block
} else {
    # Else block
}
```

And the `{ }` braces can be omitted like in `C`, in the case of single-statement
blocks. `conditionExpr` can be any expression, for example: `(a=3)+b >= c && !d`.

### Const evaluation of conditional statements

When `conditionExpr` is an expression that can be const-evaluated, the whole
if-statement is replaced by the true-branch, while the false-branch is
discarded. For example, consider the following script:

```C#
const a = 3;
const b = 4;

if (a < b) {
    print("yes");
} else {
    print("no");
}
```

Not only it always prints "yes", but it does not even need to check anything before
doing that. Check the abstract syntax tree:

```
$ ./build/mylang -s t
Syntax tree
--------------------------
Block(
  Block(
    CallExpr(
      Id("print")
      ExprList(
        "yes"
      )
    )
  )
)
--------------------------
yes
```

### Classic loop statements

`MyLang` supports the classic `while` and `for` loops.

```C#
while (condition) {

    # body

    if (something)
        break;

    if (something_else)
        continue;
}

for (var i = 0; i < 10; i += 1) {

    # body

    if (something)
        break;

    if (something_else)
        continue;
}
```

Here, the `{ }` braces can be omitted as in the case above. There are only
a few difference from `C` worth pointing out:

  - The `++` and `--` operators do not exist in `MyLang`, at the moment.

  - To declare multiple variables, use the syntax: `var a, b = [3,4];` or
    just `var a,b,c,d = 0;` if you want all the variables to have the same
    initial value.

  - To increase the value of multiple variables use the syntax:
    `a, b += [1, 2]`. In the extremely rare and complex cases when in the
    *increment* statement of the for-loop we need to assign to each variable
    a new variable using different expressions, take advantage of the
    expansion syntax in assignment: `i, j = [i+2, my_next(i, j*3)]`.

### The foreach loop

`MyLang` supports `foreach` loops using a pretty familiar syntax:

```C#
var arr = [1,2,3];

foreach (var e in arr) {
    print("elem:", e);
}
```

Foreach loops can be used for arrays, strings, and dictionaries.
For example, iterating through each `<key, value>` pair in a dictionary
is easy as:

```C#
var d = { "a": 3, "b": 10, "c": 42 };

foreach (var k, v in d) {
    print(k + " => " + str(v));
}
```

To iterate only through each key, just use `var k in d` instead.

#### Extra features: the "indexed" keyword

`MyLang` supports enumeration in foreach loops as well. Check the following
example:

```C#

var arr = ["a", "b", "c"];

foreach (var i, elem in indexed arr) {
    print("elem["+str(i)+"] = "+elem);
}
```
In other words, when the name of the container is preceded by the keyword
`indexed`, the first variable gets assigned a progressive number at each iteration.

#### Extra features: array expansion

While iterating through an array of small fixed-size arrays (think about tuples),
it's possible to directly expand those "tuples" in the foreach loop:

```C#
var arr = [
    [ "hello", 42 ],
    [ "world", 11 ]
];

foreach (var name, value in arr) {
    print(name, value);
}

# This is a shortcut for:

foreach (var elem in arr) {

    # regular array expansion
    var name, value = elem;
    print(name, value);
}

# Which is a shortcut for:

foreach (var elem in arr) {

    var name = elem[0];
    var value = elem[1];

    print(name, value);
}
```

### Functions and lambdas

Declaring a function is simple as:

```C#
func add(x, y) {
  return x+y;
}
```

But several shortcuts are supported as well. For example, in the case of
single-statement functions like the one above, the following syntax can be
used:

```C#
func add(x, y) => x + y;
```

Also, while it's a good practice to always write `()` for parameter-less
functions, they're actually optional in this language:

```C#
func do_something { print("hello"); }
```

Functions are treated as regular symbols in `MyLang` and there're no substantial
differences between standalone functions and lambdas in this language. For example,
we can declare the `add` function (above) as a lambda this way:

```C#
var add = func (x, y) => x + y;
```

Note: when creating function objects in expressions, we're not allowed to assign
them a name.

#### Const parameters

A parameter can be declared `const`, which forbids reassigning it in the body:

```C#
func f(const x, y) {
  y = y + 1;     # OK: `y` is an ordinary (mutable) parameter
  return x + y;  # `x` may be read but never reassigned
}
```

Reassigning a `const` parameter is a compile-time error. A non-const parameter
stays mutable, and - since parameters are passed by value - a function (even a
`pure` one) may freely reassign its own non-const parameters without affecting
the caller. A plain parameter that is never reassigned anywhere in the body is
treated as *effectively const* automatically.

#### Typed parameters

A parameter may carry an explicit primitive type, like a variable declaration
(see *Explicit types*): `func f(int a, str b)`. The type is enforced — every
call is checked that the argument is assignable to the declared parameter type
(`f(1, 2)` is a compile error since `2` is not a `str`), and a widening numeric
argument is coerced (`func g(float x); g(3)` binds `x = 3.0`). A typed parameter
overrides the usual "infer the type from the call sites" behavior — it *is* that
type, and callers must comply.

All the declaration forms work on a parameter — `const`, `opt`/`?`, `dyn`,
`var`, and the type keywords — so the canonical, fully-explicit style is e.g.
`func f(var x, var? y, dyn z, dyn? k, int m, int? n)`.

##### Short parameter forms

Because lambdas reuse the very same `func(params) => body` syntax (there is no
separate lambda notation), parameters also accept two **terse, param-only**
shortcuts so quick callbacks stay compact:

  * a trailing **`?`** on the *name* makes the parameter optional: `y?` ≡
    `var? y` ≡ `opt y`;
  * a leading **`~`** makes it dynamic: `~z` ≡ `dyn z`, and `~z?` ≡ `dyn? z` ≡
    `opt dyn z`.

```C#
sort(a, func(x, y) => x < y);       # plain
map(func(~x) => str(x), mixed);     # ~x : a dynamic parameter
func handler(event, ~data?) => ...  # data : optional and dynamic
```

These two shortcuts are accepted **only inside a parameter list** — a body
declaration must use the canonical `dyn x` / `var? x` forms (`~x` and a trailing
`x?` are not declaration syntax outside of `func(...)`).

#### Named arguments

At a call site an argument may be passed **by name**, with the `name: value`
syntax, for both required and optional parameters:

```C#
func f(x, y?, z?) => [x, y, z];

f(x: 1, y: 2, z: 3);   # all by name
f(x: 1, z: 3);         # skip the optional `y` -> it binds to none: [1, none, 3]
f(1, z: 3);            # positional `x`, then named `z`        -> [1, none, 3]
```

The rules are deliberately strict, so a named call reads exactly like the
declaration:

  * **Names follow parameter-declaration order.** You may *skip* an optional
    parameter, but you may not *reorder*: `f(z: 3, x: 1)` is a compile error.
    A skipped *interior* optional parameter binds to `none` (a skipped trailing
    one is simply omitted, as without names).
  * **Positional arguments come first**, then named ones — a positional
    argument after a named one is a syntax error.
  * Each parameter may be given **once**: naming a parameter already filled
    positionally (or repeating a name) is an error.
  * Every **required** parameter must still be supplied (by position or name).
  * Arguments evaluate **left to right**, exactly as written (because names are
    required to be in order, this matches a plain positional call).

Named arguments are pure syntactic sugar: the compiler rewrites the call to the
equivalent positional one (filling a skipped interior optional with `none`)
before the program runs, so they have **no effect on behavior, performance, or
any optimization** — a named call is treated exactly like the positional call it
desugars to. In particular it **const-folds** identically: a named call to an
in-scope `pure func` with constant arguments is evaluated at compile time and
may initialize a `const`, just like the positional form.

Because the rewrite needs the callee's parameter names, names are only allowed
when the callee is a **directly-named function** (a top-level/lexical `func`, or
a variable bound to a lambda). Naming arguments through an opaque callable — a
`dyn` value, a function-typed parameter, or a builtin — is a compile error.

#### Lambda captures

Lambdas support a capture list as well, but implicit captures are not supported,
in order to enforce clarity. Of course, lambdas can be returned as any other object.
For example:

```C#
func create_adder_func(val) =>
  func [val] (x) => x + val;

var f = create_adder_func(5);

print(f(1));  # Will print 6
print(f(10)); # Will print 15
```

Lambdas with captures have a *state*, as anyone would expect.
Consider the following script:

```C#
func gen_counter(val) => func [val] {
    val += 1;
    return val;
};

var c1 = gen_counter(5);

for (var i = 0; i < 3; i += 1)
  print("c1:", c1());

# Clone the `c1` lambda object as `c2`: now it will have
# its own state, indipendent from `c1`.
var c2 = clone(c1);
print();

for (var i = 0; i < 3; i += 1)
  print("c2:", c2());

print();

for (var i = 0; i < 3; i += 1)
  print("c1:", c1());
```

It generates the output:

```
c1: 6
c1: 7
c1: 8

c2: 9
c2: 10
c2: 11

c1: 9
c1: 10
c1: 11
```

### Calling functions during const-evaluation

Regular user-defined function objects (including lambdas) are not considered
`const` and, therefore, *cannot* be run during const-evaluation. That's a pretty
strong limitation. Consider the following example:

```C#
const people = [
  ["jack", 3],
  ["alice", 11],
  ["mario", 42],
  ["bob", 38]
];

const sorted_people = sort(people, func(a, y) => a[0] < b[0]);
```

In this case, the script *cannot* create the `sorted_people` const array.
Because we passed a function object to the const `sort()` builtin, we'll
get an `ExpressionIsNotConstEx` error. Sure, if `sorted_people` were
declared as `var`, the script would run, but the array won't be const anymore
and, we won't be able to benefit from any *parse-time* optimization. Therefore,
while the `sort()` builtin can be called during const-evaluation, when it has a
custom `compare func` parameter, that's not possible anymore.

### Pure functions

To overcome the just-described limitation, `MyLang` has a special syntax for
*pure* functions. When a function is declared with the `pure` keyword preceding
`func`, the interpreter treats it in a special way: it *can* be called anytime,
*both* during const evaluation and during runtime *but* the function cannot
see global variables, nor capture anything: it can only use constants and the
value of its parameters: that's exactly what we need during const evaluation.
For example, to generate `sorted_people` during const evaluation it's enough to
write:

```C#
const sorted_people = sort(people, pure func(a, b) => a[0] < b[0]);
```

Pure functions can be defined as standalone functions and *can* be used with
non-const parameters as well. Therefore, if a function can be declared as `pure`,
it should always be declared that way. For example, consider the following
script:

```C#
pure func add2(x) => x + 2;

var non_const = 25;

print(add2(non_const));
print(add2(5));
```

The abstract syntax tree that language's engine will use at runtime will be:

```
$ ./build/mylang -s t
Syntax tree
--------------------------
Block(
  FuncDeclStmt(
    Id("add2")
    <NoCaptures>
    IdList(
      Id("x")
    )
    Expr04(
      Id("x")
      Op '+'
      Int(2)
    )
  )
  VarDecl(
    Id("non_const")
    Op '='
    Int(25)
  )
  CallExpr(
    Id("print")
    ExprList(
      CallExpr(
        Id("add2")
        ExprList(
          Id("non_const")
        )
      )
    )
  )
  CallExpr(
    Id("print")
    ExprList(
      Int(7)
    )
  )
)
--------------------------
27
7
```

As you can see, in the first case an actual function call happens because
`non_const` is not a constant, while in the second case it's AS IF we passed
a literal integer to `print()`.

#### Automatic pure promotion

You don't always have to write `pure`. The interpreter promotes a function to
*effectively pure* automatically when it can prove the function is pure: it has
no capture list, reads only constants (and its own parameters), and calls only
const builtins or pure functions. Such a function reports `ispure() == true`
(but `ispuredecl() == false`), and its calls with constant arguments fold to
their result:

```C#
func add2(x) => x + 2;       # effectively pure -> auto-promoted
var k = add2(5);             # folds to 7
```

There is a subtlety in *when* the folding happens. An **explicit** `pure func`
is recognized during parsing, so its const-argument calls fold at parse time and
may even initialize a `const` (`const k = add2(5)`). An **auto-promoted**
function is recognized only after parsing, so its calls fold as part of
[auto-const](#automatic-const-promotion) — which rewrites `var` uses, not a
`const` initializer. So `const k = add2(5)` requires `add2` to be declared
`pure`; with a plain `func`, write `var k = add2(5)`.

The detection is conservative: a function that reads a non-const global, calls a
non-const builtin (`print`, `rand`, I/O, ...) or a non-pure function, captures
anything, recurses, or nests another function is left impure. When in doubt,
declare the function `pure` explicitly — that also turns "this is not pure" into
a hard error instead of a silent missed optimization.

### Exceptions

Like for other constructs, `MyLang` has an exception handling similar to
`Python`'s, but using a syntax similar to `C++`. The basic construct is
the `try-catch` statement. Let's see an example:

```C#
try {

    var input_str = "blah";
    var a = int(input_str);

} catch (TypeErrorEx) {
    print("Cannot convert the string to integer");
}
```

Note: if an exception is generated by constant expressions (e.g. `int("blah")`),
during the const-evaluation, the error will be reported directly, bypassing any
exception handling logic. The reason for that is to enforce *early failure*.

Multiple `catch` statements are allowed as well:

```C#
try {
    # body
} catch (TypeErrorEx) {
    # error handling
} catch (DivisionByZeroEx) {
    # error handling
}
```

And, in case several exceptions can be handled in with the same code,
a shorter syntax can be used as well:

```C#
try {

    # body

} catch (TypeErrorEx, DivisionByZeroEx as e) {

    # error handling
    print(e);

} catch (OutOfBoundsEx) {
    # error handling
}
```

Exceptions might contain data, but none of the built-in exceptions
currently do. The list of builtin *runtime* exceptions that can be
caught with `try-catch` blocks is:

  * DivisionByZeroEx
  * AssertionFailureEx
  * NotLValueEx
  * TypeErrorEx
  * NotCallableEx
  * OutOfBoundsEx
  * KeyNotFoundEx
  * CannotOpenFileEx

Other exceptions like `SyntaxErrorEx` cannot be caught, instead.
It's also possible in `MyLang` to catch ANY exception use a catch-anything
block:

```C#
try {

    # body

} catch {

    # Something went wrong.
}
```

### Structs

`struct` declares a C-like value type with explicitly-typed fields and
type-level `const`s (no methods in v1):

```C#
struct Point {
    int x;
    int y;
    const ORIGIN_NAME = "origin";
}

var p = Point(1, 2);          # positional
var q = Point(x: 3, y: 4);    # named
var r = Point(1, y: 2);       # mixed (positional then named)
p.x;                          # field read -> 1
p.x = 9;                      # field write
p.ORIGIN_NAME;                # const via an instance -> "origin"
Point.ORIGIN_NAME;            # const via the type     -> "origin"
```

**Construction is a call.** `Point(...)` is written exactly like a function
call and reuses the same argument rules — positional, named (`x: v`), and mixed
(positional first, then names **in field-declaration order**); a skipped
*optional* field becomes `none`. So construction is type-checked like a call: a
wrong-typed, missing, extra, reordered, or unknown-named field is a compile
error.

**Fields take the same explicit types as variables** — `bool`, `int`, `float`,
`str`, `array`, `dict`, another struct type, or `dyn`. v1 restrictions (to be
lifted later): a field must have an explicit type (no `var`), and `opt` is
allowed only on a `dyn`/`array`/`dict` field (a non-opt field is guaranteed
never `none`). An uninitialised field-less use isn't possible — every field is
supplied at construction (or defaulted to `none` when an omitted optional).

**Access.** `obj.field` reads/writes a field; `obj.CONST` / `Type.CONST` reads a
const member. `.` means *field access* on a struct and *key access* on a dict —
resolved by the base's type. Reading a field that doesn't exist is a compile
error (for a statically-typed base).

**Value semantics.** A struct is a value, with the same COW semantics as
arrays/dicts: plain assignment **aliases** (`var q = p; p.x = 9` makes `q.x` 9
too, like Python objects), while `clone()` makes an independent (shallow) copy
and `deepclone()` a deep one. `==` is structural and field-wise between
**same-type** instances (different struct types are never equal); structs are
not hashable (cannot be dict keys) in v1. `print(p)` shows `Point(x: 1, y: 2)`.

**`const` works fully.** A struct holds state only in instances, not in the
type, so `const P = Point(1, 2)` is computed at compile time and is **deep
read-only** — mutating a const instance's field is an error. `Type.CONST` folds
at parse time. An `array` of a struct type infers as `array<Struct>`
(`var a = [Point(1,2), Point(3,4)]`).

**Layout.** A struct whose fields are all `bool`/`int`/`float` (or other such
POD structs, embedded inline) gets a compact native-C byte layout, and an
`array` of it is stored **flat/unboxed** — contiguous bytes, no per-element
object — just like `array<int>` (`array_storage(a)` reports `"structs"`). A
struct with any `array`/`dict`/`str`/`dyn`/`opt` field is stored as a boxed
slot array instead. This is transparent: it changes only memory layout and
speed, never behavior.

Structs may be declared anywhere a statement is allowed, **including inside a
function** (lexically scoped like a nested function). A struct's fields and
consts live in the struct's own namespace, so a struct `const PI` never clashes
with a global `const PI`.

### Custom exceptions

This language does not support throwing custom objects, at the moment.
Therefore,
it's not possible to *throw* any kind of object like in some other languages.
To throw an exception, it's necessary to use the special built-in function
`exception()` or its shortcut, `ex()`. Consider the following example:

```C#
try {

    throw ex("MyError", 1234);

} catch (MyError as e) {

    print("Got MyError, data:", exdata(e));
}
```

As intuition suggests, with `ex()` we've created and later thrown an exception
object called `MyError`, having `1234` as payload data. Later, in the `catch` block,
we caught the exception and, we extracted the payload data using the `exdata()` builtin.

In case a given exception doesn't need to have a payload, it's possible to just
save the result of `ex()` in a variable and throw it later using a probably more
pleasant syntax:

```C#
var MyError = ex("MyError");
throw MyError;
```

### Re-throwing an exception

`MyLang` supports re-throwing an exception in the body of catch statements
using the dedicated `rethrow` keyword:

```C#
try {

    do_something();

} catch {

    print("Something went wrong!!");
    rethrow;
}
```

### The finally clause

In some cases, it might be necessary to do some clean-up, after executing
a block of code that might throw an exception. For these cases, `MyLang`
supports the well known `finally` clause, which works exactly as in `C#`:

```C#
try {

    step1_might_throw();
    step2_might_throw();
    step3_might_throw();
    step4_might_throw();

} catch (TypeErrorEx) {

    # some error handling

} finally {

    # clean-up
}
```

It's worth noting that `try-finally` constructs (without any `catch` clause) are
allowed as well.

## Builtins

### Const builtins
The following built-in functions will be evaluated during *parse-time* when
const arguments are passed to them.

### General builtins

#### `defined(symbol)`
Check if `symbol` is defined. Returns `true` if the symbol is defined, `false`
otherwise.

#### `len(container)`
Return the number of elements in the given container.

#### `str(value, [decimal_digits])`
Convert the given value to a string. If `value` is a float, the 2nd parameter
indicates the desired number of decimal digits in the output string.

#### `int(value)`
Convert the given string to an integer. If the value is a float, it will be
trucated. If the value is a string, it will be parsed and converted to an integer,
if possible. If the value is already an integer, it will be returned as-it-is.

#### `float(value)`
Convert the given value to float. If the value is an integer, it will be
converted to a floating-point number. If the value is a string, it will parsed
and converted to float, if possible. If the value is already a float, it will be
returned as-it-is.

#### `clone(obj)`
Clone the given object, **shallowly**: a non-trivial object (array, dictionary,
lambda with captures) gets a fresh top-level copy, but nested objects are
*shared* with the original. Because a nested object of a `const` is itself
read-only, `clone(constObj)[k]` can be reassigned but `clone(constObj)[k][...]`
cannot — use `deepclone()` to mutate at any depth.

#### `deepclone(obj)`
Like `clone()`, but produces a **fully mutable, deep copy**: every nested array
and dictionary is copied too, so the result is completely independent of the
original and writable at any depth. This is the way to obtain a mutable version
of a `const` (or of any object you want to change deeply without affecting the
source). Scalars and strings are returned unchanged.

#### `type(value)`
Return the name of the type of the given value in string-form (e.g. `"int"`,
`"bool"`, `"str"`, `"arr"`). Useful for debugging.

#### `hash(value)`
Return the hash value used by dictionaries internally when `value` is used
as a key. At the moment, only booleans, integers, floats and strings support
`hash()` (a `bool` hashes as the equal int `0`/`1`).

### Array builtins

#### `array(N, [value])`
Return an array with `N` elements. With a second argument, every element is
`value` (a fixed value, not a callback). With **one** argument the fill is
chosen by *type inference*: if the array's inferred element type is `int`,
`float`, or `bool` (e.g. you later fill it with ints), unfilled elements are
`0` / `0.0` / `false`; otherwise they are `none`. So `var a = array(3); a[0] =
5;` yields `[5, 0, 0]`, while an array that stays untyped/dynamic yields all
`none`. The element type also picks the internal storage: an `int`/`float`/`bool`
array is a compact *flat* array (see `array_storage()`), anything else is
general. To build each element from its index, use `make_array()`.

(`array()` is a *non-const* builtin: a call like `array(1000000)` is never
folded into a baked literal at parse time, it always allocates at run time.)

#### `make_array(N, gen_func)`
Return `[gen_func(0), gen_func(1), ..., gen_func(N-1)]` — the callback form of
`array()`. `gen_func` is called once per index with that index. The result is
flat (`int`/`float`/`bool`) when every element the callback returns is that one
scalar kind, otherwise general.

#### `top(array)`
Return the last element of the array. This is an alias for `array[-1]`.
It is useful when a given array is used as a stack, in combination with
other builtins like `push()` and `pop()`.

#### `range(n, [end, [step]])`
When only one parameter is provided, it returns an array with numbers
from 0 to `n`. When `end` is passed to the function, the array goes from
`n` to `end-1`. When `step` is passed too, the array goes from `n` to
`end-step` with each element being `step` bigger than the previous. `step`
can be negative as well. This is equivalent to `Python 2.x`'s range() function.
In `Python 3.x`, this is equivalent to: `list(range(...))`. Warning: while
it might look pretty in foreach loops, that's typically not a good idea
because it returns a whole array, not a generator object like in `Python 3.x`.
Therefore, for small ranges is fine, but for larger ranges it's better to
use the classic for-loop.

#### `sort(array, [compare_func])`
Sorts the given array *in-place* and returns the same array. Optionally,
it supports a `compare_func` parameter: when passed, it's used to compare
any two elements and it's supposed to return the logical value of `a < b`.
The comparator should be a *strict weak ordering* (return `a < b`, not a
qsort-style numeric difference like `a - b`). A comparator that isn't one
yields an **unspecified but well-defined, memory-safe** ordering — never a
crash: with a custom comparator `sort()` uses a heapsort that stays within the
array's bounds regardless of what the comparator returns.

Note: while `sort()` works in-place, it still can be used to sort arrays
without altering them and to sort const arrays as well: in the first case,
it's possible by calling it as `sort(clone(arr))` and storing its return
value to a new variable, while in the second case (const arrays), not even
`clone()` is required: in case of const arrays, it will just sort and return
a clone of the given array.

#### `rev_sort(array, [compare_func])`
Behaves exactly like `sort()`, but sorts the array in descending order.

#### `reverse(array)`
Reverse the given array in-place and returns it. Like `sort()`, if the given
argument is const, it will be cloned before reversing. Therefore, it can be
used during const-evaluation.

#### `sum(array, [key_func])`
Apply the `+` operator sequentially to all elements in the given array and
return the result. In case the optional argument `key_func` is passed to `sum()`,
the operator `+` is applied to the result of `key_func(elem)`, for each element
instead.

### Dictionary builtins

#### `keys(dictionary)`
Return an array containing all the keys of the given dictionary.

#### `values(dictionary)`
Return an array containing all the values of the given dictionary.

#### `kvpairs(dictionary)`
Return the contents of the given dictionary, in the form of an array of
[key, value] arrays.

#### `dict(array)` / `dict(default_value)`
With an **array** argument, build a dictionary from an array of [key, value]
arrays (the counterpart of `kvpairs()`). With any other (non-array, non-`none`)
argument, build an empty **default dict**: reading a missing key returns (and
inserts) that default value instead of throwing, so accumulation works directly:

```C#
var counts = dict(0);
foreach (var w in words)
    counts[w] += 1;        # a missing word reads as 0
```

#### `get(dictionary, key)`
Look up `key`, returning its value or `none` if the key is absent — the explicit
*nullable* lookup (type `opt V`, so you narrow it before use). Contrast
`d[key]`, which throws on a missing key.

#### `get!(dictionary, key)`
Look up `key`, returning its value or raising `KeyNotFoundEx` if the key is
absent — the *fail-fast* lookup (type `V`, non-`opt`, so the result is usable
without a none-check). Same behavior as the `d[key]` / `d.key` sugar.

### String builtins

#### `split(string, delim)`
Split the given string by the given delimiter. Returns an array.

#### `join(array_of_strings, delim)`
Join the given array of strings with the given delimiter. Returns a string.

#### `ord(string)`
Return the numeric value of the given 1-char string.
Note: in MyLang chars are 8-bit wide and there's no Unicode support,
to keep this educational project smaller and simpler.

#### `chr(num)`
Return a 1-char string containing the string representation of the given
number in the range [0, 255]. Note: in MyLang chars are 8-bit wide and there's
no Unicode support, to keep this educational project smaller and simpler.

#### `splitlines(string)`
Split the given string, line by line. Returns an array. It's different
from `split(string, "\n")` because it handles multiple types of line ending
sequences.

#### `lpad(string, n, [char])`
Add left-padding to the given string to make it long `n` chars.
If len(string) >= n, return the input string as it is. If a 3rd argument
is passed to `lpad()`, it will be used as padding-character. By default,
the padding character is space.

#### `rpad(string, n, [char])`
The counter-part of `lpad()`: pad the string on the right.

#### `lstrip(string)`
Return a slice of the given string skipping any leading whitespace.

#### `rstrip(string)`
Return a slice of the given string skipping any trailing whitespace.

#### `strip(string)`
Return a slice of the given string skipping any leading or trailing
whitespace.

#### `startswith(string, sub_string)`
Return `true` if the given string starts with the given sub_string, `false`
otherwise.

#### `endswith(string, sub_string)`
Return `true` if the given string ends with the given sub_string, `false`
otherwise.


#### Generic-container builtins

#### `find(container, what, [key_func])`
Generic *find* function working with strings, arrays, and dictionaries.
When `container` is a string, it returns the index of the first occurrence
of the `what` substring in `container` or `none`.

When `container` is an array, it returns the index of the first element equal
to `what`. Also, when `container` is an array, a 3rd parameter (`key_func`) is
supported: it's a function object accepting a value (element of the array) and
returning the value that must be compared to `what`. It's useful when we're
searching something in an array of composite elements (e.g. tuples).

When `container` is a dictionary, it returns the value associated with the
given key (`what`) or `none` otherwise.

#### `map(func, container)`
Map each element in `container` through `func(elem)` and return the resulting
array. For example, the following identity holds:

```
map(func(x) => x+1, [1, 2, 3]) == [2, 3, 4]
```

In case the container is a dictionary, `func` is required to accept two arguments,
a key and a value, but the result will still be an array. For example:

```
map(func(k, v) => [k, v+1], {"a": 3, "b": 4}) == [["a",4],["b",5]]
```

#### `filter(func, container)`
Filter the elements of `container` through `func(elem)` and return a container
of the same type. For example:

```
filter(func(x) => x > 3, [1, 2, 3, 4, 5]) == [4, 5]
```

In case the container is a dictionary, `func` is required to accept two parameters,
a key and a value, but the behavior will be semantically the same (a dictionary will
be returned).

### Numeric builtins

#### `abs(num)`
Return the absolute value of the given number.

#### `min(a, b, [c, [...]])`
Return the smallest value among the ones passed to it.

#### `min(array)`
Return the smallest value among the ones in the given array.

#### `max(a, b, [c, [...]])`
Return the largest value among the ones passed to it.

#### `max(array)`
Return the largest value among the ones in the given array.

#### `exp(x)`
Return e^x.

#### `exp2(x)`
Return 2^x.

#### `log(x)`
Return the natural logarithm of `x`.

#### `log2(x)`
Return the base-2 logarithm of `x`.

#### `log10(x)`
Return the base-10 logarithm of `x`.

#### `sqrt(x)`
Return the square root of `x`.

#### `cbrt(x)`
Return the cube root of `x`.

#### `pow(x, y)`
Return x^y.

#### `sin(x)`
Return `sin(x)`.

#### `cos(x)`
Return `cos(x)`.

#### `tan(x)`
Return `tan(x)`.

#### `asin(x)`
Return the arc sine of `x`.

#### `acos(x)`
Return the arc cosine of `x`.

#### `atan(x)`
Return the arc tangent of `x`.

#### `ceil(x)`
Return the smallest integral value that is not less than `x`.

#### `floor(x)`
Return the largest integral value that is not greater than `x`.

#### `trunc(x)`
Return the rounded integer value of `x` as float.

#### `isinf(x)`
Return true if `x` is `inf` or `-inf`.

#### `isfinite(x)`
Return true if `x` is a finite value.

#### `isnormal(x)`
Return true if `x` is a normal floating-point number.

#### `isnan(x)`
Return true if `x` is "Not a Number".

#### `round(x, [precision])`
Round `x` to the nearest integer or to a floating-point number with
`precision` digits.

### Numeric constants

 Constant name         | Value
-----------------------|-------------------------
 math_e                | Euler's number
 math_log2e            | log2(e)
 math_log10e           | log10(e)
 math_ln2              | log(2)
 math_ln10             | log(10)
 math_pi               | pi
 math_pi2              | pi/2
 math_pi4              | pi/4
 math_1_pi             | 1/pi
 math_2_pi             | 2/pi
 math_2_sqrt_pi        | 2/sqrt(pi)
 math_sqrt2            | sqrt(2)
 math_1_sqrt2          | 1/sqrt(2)
 nan                   | Not a Number
 inf                   | Infinity
 eps                   | Floating-point's epsilon


### Non-const builtins
The following built-in functions will *not* be evaluated during *parse-time*,
no matter if const arguments are passed to them or not.

### Non-const misc builtins

#### `assert(expr)`
Check `expr` and throw AssertionFailureEx if it's false.

#### `exit(code)`
Exit the program with the given numeric code

#### `runtime(expr)`
An *optimization barrier*. Returns the value of its single argument unchanged at
runtime, but because it is a non-const builtin, the call is opaque to
const-folding and *auto-const* (see [Declaring
constants](#declaring-constants)): any expression that contains `runtime(x)` is
never folded and is therefore evaluated — and any exception it raises thrown —
at runtime instead of at "compile" time. Note that the *argument* is still
folded normally, so `runtime(1/0)` fails at compile time (the error is *inside*
the expression, before it is "runtime-ized"), whereas `1 / runtime(0)` throws a
catchable `DivisionByZeroEx` at runtime. Useful for tests and to deliberately
opt a specific expression out of folding.

#### `isconst(expr)` / `isconstdecl(expr)`
Compile-time introspection (mainly handy in tests). `isconst()` is true when
`expr` is *effectively* a compile-time constant: a literal, an explicit `const`,
a constant expression, a variable promoted by
[auto-const](#automatic-const-promotion), or a `const`/auto-const parameter.
`isconstdecl()` is stricter — true only when `expr` is constant by
*declaration*: an explicit `const`, a `const` parameter, or a
literal/constant expression, but NOT a variable that is constant merely via
auto-const. So `const c = 1` gives `isconstdecl(c) == true`, while a write-once
`var v = 1` gives `isconst(v) == true` but `isconstdecl(v) == false`.

#### `ispure(func)` / `ispuredecl(func)`
`ispure()` is true when `func` evaluates to a function object that is
*effectively* pure — declared `pure`, or proven pure by the interpreter (see
[Automatic pure promotion](#automatic-pure-promotion)). `ispuredecl()` is true
only when the function was *explicitly* declared `pure`. The argument is
evaluated, so it must be a function object.

#### `intptr(symbol)`
Get the internal shared object pointer referred by `symbol`.
It's currently used in tests to check if two array slices refer internally
to the same object.

#### `array_storage(array)`
Return the array's internal storage as a string: `"ints"`, `"floats"`, or
`"bools"` for a compact *flat* (unboxed) array (8 bytes per element for
int/float, **one byte** per element for bool), or `"general"` otherwise. This
is purely an introspection aid (mainly for tests) — flat and general arrays
behave identically; the only observable difference is speed and memory.

An array's storage is **decided once, at creation, from its proven static
type** — it is never converted afterward (no runtime "promotion", so no
GC-stutter-like latency spikes). The compiler infers an array's type from *all*
its uses: an array you only ever fill with ints is `array<int>` and is born
flat; an array you also store a string into is `array<dyn>` and is born general
from the very first element — even if its initializer looked like all ints (so
`var a = [1,2,3]; a[0] = "x";` makes `a` general from the start). Every
operation (`append`/`pop`/`insert`/`erase`/`sort`/`map`/`filter`/slicing/…)
preserves the representation.

Because the representation is fixed, the *only* way to ask a flat
(statically-typed) array to hold a value of a different type is to launder it
through a `dyn` alias and mutate that (e.g. `var dyn d = int_array;
append(d, "x")`). The array's shared storage stays int-typed, so this raises a
`TypeError` rather than promoting.

To get a polymorphic (general) array on purpose:

  * **Declare it `dyn`.** `var dyn a = [1, 2, 3];` builds a general array from
    the start, so `a[0] = "x";` just works. (`runtime()` does **not** do this —
    it only changes the array's *static type* as the compiler sees it, not how
    it is stored, so the value stays flat and a mixed write still throws.)
  * **Promote an existing array with `dynarray()`** (below) — `clone()` /
    `deepclone()` deliberately preserve the layout, so a clone of a flat array
    is still flat.

#### `undef(symbol)`
Undefine the given symbol from the current scope. Return true if the given
symbol was actually defined in the given scope.

#### `exception(type_string, [payload_data])`
Create an exception object having the given type plus the optional
payload data.

#### `ex(type_string, [data])`
A shortcut for `exception()`.

#### `exdata(ex)`
Get the payload data from the given exception object.

### Non-const array builtins

#### `append(array, value)`
Append `value` to the given array.

#### `push(array, value)`
An alias for `append()`. Useful for symmetry when used with `pop()`.

#### `pop(array)`
Pop (and return) the last element from the given array.

#### `dynarray(array)`
Return a fresh, **general (polymorphic) copy** of `array` — its static type is
`array<dyn>`, so the copy can hold elements of any type. This is the explicit,
manual way to "promote" a flat (`int`/`float`) array into one you can store
mixed types into; there is no automatic runtime promotion (see
[`array_storage()`](#array_storagearray)). The original is left untouched and
keeps its compact layout, and the copy is independent of it (unlike a plain
`var d = a`, which aliases). The copy is shallow (top-level): nested arrays keep
their own representation. Compare `var dyn a = [...]`, which builds a general
array from the start.

### Non-const generic-container builtins

#### `erase(container, key_or_index)`
Erase the element at the given index or the element indexed by the given key
from the given container (array or dictionary). Return `true` if the key
existed. For arrays, it always returns `true` or throws `OutOfBoundsEx` in
case of an invalid index.

#### `insert(container, key_or_index, value)`
Insert the given value in the given container at the given index or key,
depending on the type of the container. As `erase()`, it returns true if
the insertion was successful, in the case `container` is a dictionary.
In the case `container` was an array, always return true or throws
`OutOfBoundsEx`.

### Non-const numeric builtins

#### `rand(a, b)`
Generate a random integer in the range [a, b].

#### `randf(a, b)`
Generate a random floating-point number in the range [a, b].

### Non-const I/O builtins

#### `print(a, [b, [c, [...]]])`
Write to the standard output the string-versions of the given
arguments, separated by a single space and terminated by a line ending.

#### `readln()`
Read a single line from the standard input.

#### `writeln(string)`
Write the given string to the standard output, plus a line ending
sequence.

#### `read([filename])`
If no arguments were given to `read()`, it returns the whole data in the
standard input. Otherwise, read the whole given file and returns it as a
string.

#### `write(string, [filename])`
Write the given string to the standard output as it is, without a line ending.
If the optional parameter `filename` is given, write the given string to the
given file, as it is.

#### `readlines([filename])`
Similar to `read()`, but read line by line and return an array.

#### `writelines(array_of_strings, [filename])`
Similar to `write()`, but accept an array of strings and write them one
per line.

#### `remove(filename)`
Delete a file. Returns `true` if a file was removed, `false` otherwise (e.g. it
did not exist), so it is safe to call for cleanup without checking first. Throws
only on a bad argument (a non-string, or the wrong number of arguments).

#### `tmpdir()`
Return the OS temporary directory as a string, with no trailing separator (so
you can append `"/name"`). Portable: honors `$TMPDIR` / `%TEMP%` / `%TMP%`,
falling back to `/tmp`. Like Python's `tempfile.gettempdir()`.
