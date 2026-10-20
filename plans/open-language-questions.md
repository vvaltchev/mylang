# Open language questions

Questions raised while fixing the broken CI runs (2026-10-12), decided
one at a time with the maintainer. Each entry says what happens today,
the options, the recommendation and - once taken - the decision.

Already decided in that session (done, pushed): delete the redundant
INT units and keep the gate red on a coverage regression; dicts keep
insertion order (#50); a `dyn` value entering a typed place is checked
at run time (#49).

| #  | question                                        | status     |
|----|-------------------------------------------------|------------|
| 3  | does a slice alias like any other array?        | decided: B |
| 5  | a parse-time pure call runs before inference    | decided: A |
| 6  | a throwing constant expression inside a `try`   | decided: A |
| 7  | duplicate keys in a dict literal                | decided: A |

## 3. Does a slice alias like any other array once assigned?

**The two rules, side by side** (`var s = a[1:3];` throughout):

- **Taking the slice.** In Python, `a[1:3]` builds a NEW list holding
  copies of the two elements. In MyLang, `a[1:3]` builds no new array:
  `s` is a VIEW, a window onto `a`'s storage. Either way a later write
  to `a` does not show in `s` (MyLang copies the window out first).
- **Assigning it, `var t = s;`.** In Python, `t` is a second name for
  the SAME list, so `t[0] = 9` makes `s` read `[9, 3]` too. In MyLang,
  `t` is a second, INDEPENDENT view of the window; `t[0] = 9` first
  copies the window into a new array that only `t` holds, then writes
  there, so `s` still reads `[2, 3]`.
- **Passing it, `f(s)`.** In Python, the parameter is the SAME list,
  so a write inside `f` is seen by the caller. In MyLang, the parameter
  is another independent view; a write inside `f` goes into a private
  copy that is thrown away when `f` returns, so the caller's `s` is
  unchanged.
- **Storing it, `var box = [s];`.** In Python, `box[0]` is the SAME
  list as `s`. In MyLang, `box[0]` is another independent view, and
  every read of it gives yet another.
- **After `s` itself is written.** In MyLang, the first write or
  append to `s` (`s[0] = 9`, `append(s, 5)`, ...) copies the window into
  a new array that `s` holds; `s` is then an ordinary array, and from
  then on `t = s`, `f(s)` and `[s]` SHARE it, as in Python. In Python
  nothing changes at that point: `s` was an ordinary list all along.

In short: Python copies ONCE, when the slice is taken, and the result is
an ordinary list from then on. MyLang copies at every assignment,
argument and store for as long as `s` has never been written, and
behaves like Python once it has.

Measured on 8078768c (`build-claude/dbg`, the same in `-tw`, `-nj` and
the default engine) against python3:

| case                                         | MyLang        | Python        |
|----------------------------------------------|---------------|---------------|
| 1 `b = a; b[0] = 100`                        | shared        | shared        |
| 2 `s = a[1:3]; s[0] = 9` (source unchanged)  | separate      | separate      |
| 3 `s = a[1:3]; t = s; t[0] = 9`              | s stays [2,3] | s is [9,3]    |
| 4 `setz(s)` writes `x[0] = 0` into its param | s stays [2,3] | s is [0,3]    |
| 5 `t = s; append(t, 5)`                      | s stays [2,3] | s is [2,3,5]  |
| 6 `f = a[:]; g = f; g[0] = 9`                | f unchanged   | f changes     |
| 7 `box = [a[1:3]]; u = box[0]; u[0] = 9`     | box unchanged | box changes   |
| 9 `s[0] = 9` first, then `t = s; t[1] = 8`   | shared        | shared        |
| 10 `append(s, 5)` first, then `setz(s)`      | s changes     | s changes     |
| 11 `setz(s)` on an untouched slice           | s unchanged   | s changes     |

Cases 10 and 11 are the same function call on the same kind of value:
it changes the caller's array or not depending on whether the slice was
appended to before. `README.md` ("Array slices behave like copies") and
`bench/README.md` ("identical to Python's") both claim Python's
behaviour; cases 3-7 and 11 contradict them. Strings are immutable, so
the question does not arise for them.

Options:

- **A. Keep it.** For: an untouched slice never changes because another
  name was written; read-only slicing stays O(1); no work. Against:
  sharing depends on a value's history (cases 10/11); it contradicts
  the README, the bench README and Python; it is an optimizer trap
  (the AST inliner pasted a slice-holding name into a written-through
  parameter, fixed 2026-10-12).
- **B. Python's rule.** A slice is a separate array when it is TAKEN;
  every later copy shares it, like any array. Lazily: the view stays,
  but its window moves into a shared object, so all names holding it
  see one array, and the first write through any of them detaches that
  one shared array from the source. For: one rule for every array;
  removes the bug class. Against: engine and JIT work (the window is
  per-handle today, in `SharedArrayObj`); the read-only slice benches
  may move; a program relying on today's copy changes behaviour.

Recommendation: B, if the slice benches (15, 16, 29) stay flat.

**Decision (maintainer, 2026-10-18): B, Python's rule, keeping the
copy-on-write view internally.** Implemented the same day:

- every slice handle carries a GROUP (its identity among the views of its
  storage); a handle copy keeps it, a slice expression takes a fresh one.
  The storage's registry of views is keyed by group;
- a write through any handle of a slice (`materialize_slice`) gives the
  whole group ONE fresh copy of the window, so every name holding the
  slice sees it; a write to the source (`clone_aliased_slices`)
  materializes each overlapping slice once. `pop`/`erase` no longer narrow
  one handle's window, and an empty slice is a fresh array (it used to be
  the shared `empty_arr`, which would have made every empty slice ONE
  array);
- a foreach over a slice checks for a shift once the slice was
  materialized under it (C++ `fe_shifted`, the JIT's inline
  ArrEpochCheck);
- the loop slice hoister refuses an ARRAY slice that escapes the body
  (identity is now observable); string slices are unaffected.

Tests: `tests/functional/96_slice_alias.my` (every case of the table
above, all engines), the `slice:` `-rt` cases, `slice_group_identity`,
the hoister's shape and layer-equivalence cases, and the old-rule tests
rewritten (pop/erase on a slice, the struct slice field, W5,
91_inline_slice_param). Each new piece was watched failing with the old
behaviour put back.

## 5. A parse-time `pure` call runs before type inference

A `pure` function called with constant arguments is RUN by the parser,
before inference exists. That run uses each value's type at that
moment; the same call at run time uses the types inference proved. When
they disagree, the answer depends on whether the argument was constant:

```
pure func f(int n) { var x = n; var r = str(x); x = 2.5; return r; }
print(f(3), f(int(runtime(3))))    # 3 3.000000
                                   # (with -nc: 3.000000 3.000000)
pure func g(int n) { if (n > 0) { return 1; } return 2.5; }
print(g(1), g(int(runtime(1))))    # 1 1.000000
pure func h(int n) { var a = [n]; a[0] = 0.5; a[0] = n; return str(a); }
print(h(int(runtime(3))))           # [3.000000]
print(h(3))                         # adding this line stops the program
                                    # compiling
```

Proposal: type-check a pure function for its argument types before
running it at parse time, and give the folded result the call's type
(a medium parser/inferencer change). Tracked as task #51.

Decision (maintainer, 2026-10-18): A. A pure call the parser runs at
compile time is first type-inferred for its argument types, so it runs
with the types the same call has at run time: constant and run-time calls
give the same answer, and `-nc` changes nothing.

Done (2026-10-18). The three examples above now print `3.000000
3.000000`, `1.000000 1.000000` and `[3.000000]` twice, with and without
`-nc`. A call the parser makes runs AS WRITTEN in a synthetic program
(the call, the functions it reaches, its consts - one holding a function
declared by its own initializer - and struct names), which inference
types first - a builtin call given a function too, so a callback's
parameter takes the container's element type; a call a running function
makes runs by value. Details and the rules each part keeps are in
CLAUDE.md, *A USER FUNCTION THE PARSER RUNS, RUNS TYPED*.

What it decides beyond the question:

- a function reached through a VALUE is typed the way the program types
  it: through a name bound to it (`const G = show;`, a function another
  returns, an argument) a template gets its instance; read out of a
  const container (`const OPS = [show]; OPS[0](4)`) it runs untyped,
  because the program cannot redirect a baked value to an instance;
- a type error in the typed copy is a compile error: `pure func
  bad(x) { var y = x; y = "s"; return 1; }` makes `const Z = bad(3);`
  an error, as `bad(int(runtime(3)))` always was (it used to compile
  and print 1);
- under `-nti` both compile-time and run-time calls run untyped.

Known limit: a type the program joins over the whole program (an
un-annotated `opt` parameter, a lambda's unless it is written as a
builtin's argument, a template's instance through a value, which the
program makes only when every call through it agrees) is decided by the
compile-time call alone.

Found on the way (pre-existing, tasks #94-#96): a const bound to a
builtin is undefined inside a function body; a struct name used as a
value is typed `none` - a `dyn` parameter given one is refused, the VM
aborts calling a template given one, and a typed copy cannot type a call
a running function makes with one (it runs untyped, the one divergence
left: `OPT[k](P, 3)` through a const array); a compile warning's location -
and a compile error's - moves with folding (a folded call's site is
gone from the program inference sees).

Tests: `parse_time_typed_runs` (`-rt`, every engine, folding on and
off, with counters proving the copy ran), the `repl:` cross-input case,
`tests/functional/97_parse_time_typed.my`, a `driver_checks` case for
the driver's `-nti` wiring and the `.myv` image.

## 6. A constant expression that throws is a compile error inside `try`

A constant expression is evaluated while compiling; if it throws, the
throw is a compile error, so a surrounding `try`/`catch` never runs:

```
pure func half(int x) => 10 / x;
try { print(half(0)); } catch (DivisionByZeroEx) { print("caught"); }
```

This does not compile. `10 / 0` written directly behaves the same, and
so does a plain `func half` (it is auto-pure). `runtime(0)` is the only
workaround.

Measured on a74901bd (default, `-nc` and `-tw` alike): the same holds in
a branch that can never run - `if (runtime(false)) { print(half(0)); }`
does not compile - and for a constant container read, `const A = [1, 2];
try { print(A[5]); } catch (OutOfBoundsEx) { ... }` is a compile-time
OutOfBoundsEx. `half(runtime(0))` prints "caught".

- **A.** Keep it; the error message suggests `runtime()`.
- **B.** Inside a `try` that has a `catch`, leave a throwing constant
  expression to run time (recommended).
- **C.** Leave every throwing constant expression to run time with a
  compile warning; it stays an error only where a value is required at
  compile time (a `const` declaration).

Decision (maintainer, 2026-10-20): A. A throwing constant expression stays
a compile error everywhere - inside a `try`, in a branch that never runs -
and the message says why and names `runtime()`:

```
note: a constant expression is evaluated while compiling, so its error is a
compile error, which no try/catch handles; to raise it when the program runs
instead, pass one of its values through runtime(), as in 10 / runtime(0)
```

Done (2026-10-20). The note follows any catchable run-time kind of error
raised before the program runs (the script driver, `-nr`, `-c`) or while a
REPL input compiles (`format_const_eval_note`, errfmt.cpp); a type error,
a syntax error, an undefined name and every run-time error get none. Every
engine and `-nc` / `-nti` print it alike, since all of them evaluate the
constant expression while compiling. Tests: a `driver_checks` case (every
compiling mode and `-c` print it; a type error, a run-time error from
source and from an image do not) and a `repl:` case.

## 7. Duplicate keys in a dict literal

```
print({"a": 1, "a": 2});              # {"a": 1}  - the first value wins
print(dict([["a", 1], ["a", 2]]));    # {"a": 2}  - the last value wins
```

Python keeps the last value in both.

- **A.** The last value wins everywhere, as in Python and `dict()`
  (recommended).
- **B.** The first value wins everywhere.
- **C.** As A, and a repeated CONSTANT key in a literal is a compile
  error.

Decision: **A** (2026-10-20). A literal is built like successive
stores: the last value wins and the key keeps its first position, as
in Python and as `dict()` already does. `{"a": 1, "b": 2, "a": 3}`
gives `{"a": 3, "b": 2}`.

Done (2026-10-20). Every engine builds a literal through one function,
`build_dict_from_pairs` (eval.cpp: the tree-walker's `LiteralDict`, the
parser's fold of a constant literal, the VM's and the JIT's
`MakeDictV`), which now stores each pair with `insert_or_assign`, as
`dict(pairs)` does. Every key and value is still evaluated, left to
right. Tests: an `-rt` case (constant and run-time keys, the evaluation
order, a literal in a compiled loop) and
`tests/functional/98_dict_literal_dup_keys.my`, alike in every engine
and under `-nc`, `-nti` and `--no-opt all`.
