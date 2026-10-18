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
| 3  | does a slice alias like any other array?        | discussing |
| 5  | a parse-time pure call runs before inference    | open       |
| 6  | a throwing constant expression inside a `try`   | open       |
| 7  | duplicate keys in a dict literal                | open       |

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

Decision: pending.

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

Decision: pending.

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

- **A.** Keep it; the error message suggests `runtime()`.
- **B.** Inside a `try` that has a `catch`, leave a throwing constant
  expression to run time (recommended).
- **C.** Leave every throwing constant expression to run time with a
  compile warning; it stays an error only where a value is required at
  compile time (a `const` declaration).

Decision: pending.

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

Decision: pending.
