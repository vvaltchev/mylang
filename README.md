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
    * [Declaring constants](#declaring-constants)
    * [Type system](#type-system)
    * [Conditional statements](#conditional-statements)
      - [Const evaluation](#const-evaluation-of-conditional-statements)
    * [Classic loop statements](#classic-loop-statements)
    * [The foreach loop](#the-foreach-loop)
      - [The "indexed" keyword](#extra-features-the-indexed-keyword)
      - [Array expansion](#extra-features-array-expansion)
    * [Functions and lambdas](#functions-and-lambdas)
      - [Lambda captures](#lambda-captures)
      - [Calling functions during const-evaluation](#calling-functions-during-const-evaluation)
      - [Pure functions](#pure-functions)
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

  - The keywords `true` and `false` exist, but there's no `boolean` type. Like in C,
    `0` is false, everything else is `true`. However, strings, arrays, and dictionaries
    have a boolean value, exactly like in `Python` (e.g. an empty array is considered
    `false`). The `true` builtin is just an alias for the integer `1`.

  - The assignment operator `=` can be used like in `C`, inside expressions, but
    there's no such thing as the comma operator, because of the array-expansion
    feature.

  - MyLang supports both the classic `for` loop and an explicit `foreach` loop.

  - MyLang does not support custom types, at the moment. However, dictionaries
    support a nice syntactic sugar: in addition to the main syntax `d["key"]`,
    for string keys the syntax `d.key` is supported as well.

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
and dictionaries with the exception that the latter ones are instanted as at
runtime as well, in order to avoid having potentially huge literals everywhere.
Consider the following example:

```
$ ./build/mylang -s -e 'const ar=range(4); const s=ar[2:]; print(ar, s, s[0]);'
Syntax tree
--------------------------
Block(
  ConstDecl(
    Id("ar")
    Op '='
    LiteralArray(
      Int(0)
      Int(1)
      Int(2)
      Int(3)
    )
  )
  ConstDecl(
    Id("s")
    Op '='
    LiteralArray(
      Int(2)
      Int(3)
    )
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

As you can see, the *slice* operation has been evaluated at *parse-time* while
initializing the constant `s`, but both the arrays exist at runtime as well.
The subscript operations on const expressions, instead, are converted to
literals. That looks like a good trade-off for performance: small values like
integers, floats, and strings are converted to literals during the *const evaluation*,
while arrays and dictionaries (potentially big) are left as read-only symbols at
runtime, but still allowing some operations on them (like `[index]` and `len(arr)`)
to be const-evaluated.

### Type system

`MyLang` supports, at the moment, only the following (builtin) types:

  * **None**
    The type of `none`, the equivalent of Python's `None`. Variables just declared
    without having a value assigned to them, have value `none` (e.g. `var x;`).
    The same applies to functions that don't have a return value. Also, it's
    used as a special value by builtins like `find()`, in case of failure.

  * **Integer**
    A *signed* pointer-size integer (e.g. `3`).

  * **Float**
    A floating-point number (e.g. `1.23`). Internally, it's a long double.

  * **String**
    A string like "hello". Strings are immutable and support slices (e.g.
    `s[3:5]` or `s[3:]` or `s[-2:]`, having the same meaning as in `Python`).

  * **Array**
    A mutable type for arrays and tuples (e.g. `[1,2,3]`). It can contain items
    of different type and it supports writable slices. Array slices behave like
    copies while, under the hood, they use copy-on-write techniques.

  * **Dictionary**
    Dictionaries are hash-maps defined using `Python`'s syntax: `{"a": 3, "b": 4}`.
    Elements are accessed with the familiar syntax `d["key-string"]` or `d[23]`, can
    be looked-up with `find()` and deleted with `erase()`. At the moment, only strings,
    integers, and floats can be used as keys of a dictionary. **Perks**: identifier-like
    string-keys can be accessed also with the "member of" syntax: `d.key`.

  * **Function**
    Both standalone functions and lambdas have the same object type and can be passed
    around like any other object (see below). But, only lambdas can have a capture list.
    Regular functions cannot be executed during const-evaluation, while `pure` functions
    can. Pure functions can only see consts and their arguments.

  * **Exception**
    The only type of objects that can be thrown. To create them, use the `exception()`
    builtin or its shortcut `ex()`.

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

### Custom exceptions

This language does not support custom types, at the moment. Therefore,
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
Check if `symbol` is defined. Returns 1 if the symbol is defined, 0 otherwise.

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
Clone the given object. Useful for non-trivial objects as arrays,
dictionaries and lambda with captures.

#### `type(value)`
Return the name of the type of the given value in string-form.
Useful for debugging.

#### `hash(value)`
Return the hash value used by dictionaries internally when `value` is used
as a key. At the moment, only integers, floats and strings support `hash()`.

### Array builtins

#### `array(N)`
Return an array of `none` values with `N` elements.

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

#### `dict(array)`
Build a dictionary from an array of [key, value] arrays. This is the counter-part
function of `kvpairs()`.

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
Return true (1) if the given string starts with the given sub_string and
false (0), otherwise.

#### `endswith(string, sub_string)`
Return true (1) if the given string ends with the given sub_string and
false (0), otherwise.


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

#### `intptr(symbol)`
Get the internal shared object pointer referred by `symbol`.
It's currently used in tests to check if two array slices refer internally
to the same object.

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

### Non-const generic-container builtins

#### `erase(container, key_or_index)`
Erase the element at the given index or the element indexed by the given key
from the given container (array or dictionary). Return true (1) if the key
existed. For arrays, it always returns true or throws `OutOfBoundsEx` in
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
