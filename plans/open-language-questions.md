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
| 8  | `rethrow` inside a `try` nested in a `catch`    | decided: A |
| 9  | the element type of an empty container          | decided: C |
| 10 | `deepclone` and shared instances                | decided: B |
| 11 | the value of a multi-assignment                 | decided    |
| 12 | exception chaining                              | decided    |
| 13 | fixed builtin arities (`write` / `fwrite`)      | open       |
| 14 | the struct deferrals (var/opt fields, methods,  | open       |
|    | subtyping)                                      |            |
| 15 | a closure's parameters from every call site     | open       |
| 16 | read-only views of a mutable container          | open       |
| 17 | reference cycles, part B (reclaiming them)      | open       |
| 18 | `isdefined()` for a name declared nowhere       | open       |
| 19 | a stronger `--strict` (forward declarations)    | open       |
| 20 | trace verbosity levels                          | open       |

8-20 were added on 2026-10-24, in priority order, and are presented
three at a time. 11 and 15-17 have their own plan files
(`multi-assign-value.md`, `closure-param-join.md`, `immutable-views.md`,
`reference-cycles.md`); 19 is in CLAUDE.md, under `--strict`.

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
compile-time call alone. Narrowed on 2026-10-21 by #96: a constant call
inside an expression is folded only once the program is typed, and runs
when the program types it otherwise; the limit remains where the value
is needed while compiling (a const initializer, a decided condition, a
struct's const member) and for a join the call reaches only inside the
function it calls.

Found on the way (pre-existing, tasks #94-#96), all three fixed on
2026-10-21: a const bound to a builtin was undefined inside a function
body (a read of it is now that builtin); a struct name used as a value
was typed `none` - a `dyn` parameter given one was refused, the VM
aborted calling a template given one, and a typed copy could not type a
call a running function made with one (it is a `dyn` now, passed by its
name in a copy); a compile warning's location - and a compile error's,
and a joined parameter's type - moved with folding (a folded call's site
was gone from the program inference saw; such a call is now folded
after inference).

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

## 8. `rethrow` inside a `try` nested in a `catch` body

The engines disagree today (RULE 2), and the README does not say which
is right:

```
struct E { int v; }
func f() {
    try {
        throw E(1);
    } catch (E as e) {
        try {
            rethrow;
        } catch (E as e2) {
            print("inner caught", e2.v);
        }
        print("after inner try");
    }
    print("f returns");
}
try {
    f();
    print("back in main");
} catch (E as e) {
    print("outer caught", e.v);
}
```

Default engine and `-nj`: `inner caught 1`, `after inner try`, `f
returns`, `back in main` - what Python (`raise`) and C# (`throw;`)
print. `-tw`: `outer caught 1` - the rethrow skips the inner `try`.

- **A.** `rethrow` raises the exception again at that point, so the
  nearest enclosing handler (here the inner `catch`) gets it, like
  Python and C# (recommended).
- **B.** `rethrow` always leaves the `catch` it belongs to, skipping any
  `try` nested inside it.

Decision (maintainer, 2026-10-25): **A.** Done the same day: the
tree-walker's `rethrow` raises the exception the innermost running catch
body of its invocation handles (`FlowState::handling`, set by
`do_catch`), where it used to throw a private `RethrowEx` that only that
catch's own `do_catch` converted. README states the rule. Tests:
`tests/functional/102_rethrow_inner_try.my` (an inner catch, a nested
catch of another exception, a non-matching inner catch, a finally, a
catch-all with a built-in exception, a called function's own catch) and
an `-rt` case.

## 9. The element type of an empty container

`[]` is typed `array<none>` and `{}` `dict<none,none>`, so a loop over
one types its variables `none`, and any use of them is a compile error
- in a body that can never run:

```
func show(d) { foreach (k, e in d) print(k + ":"); }
show({});              # NullabilityEx at compile time
```

It also poisons a function that is otherwise fine: `show({"a": 1});
show({});` is refused too (the join makes `k` a `str?`), while
`show({"a": 1})` alone runs. `var a = []; foreach (x in a) print(x +
1);` is refused the same way. An annotated parameter
(`dict<str,int> d`) or an `append` before the loop avoids it.

- **A.** Keep it: annotate, or do not pass an empty literal.
- **B.** An empty literal's element type adds nothing to a join, so
  `show({"a": 1}); show({})` types `k` as `str`; alone it is `dyn`
  (recommended).
- **C.** An empty literal's element type is `dyn` everywhere.

Decision (maintainer, 2026-10-25): **C**, refined: an empty literal takes
the element type of what is put into it (`var a = []; append(a, 1)` stays
`array<int>`); one nothing typed is ever put into is `array<dyn>` /
`dict<dyn,dyn>`, the most generic form. "We cannot prevent at compile
time iteration over empty arrays or dicts: that would be wrong."

Done the same day. The inferencer keeps `none` as an empty container's
component while it runs (joins absorb it, so a fill still types it), but
a READ of such a component - an element, a key, a value, a foreach
variable - waits for a later round instead of typing the reader `none`,
and a reader nothing fills finalizes `dyn`; the settled container types
and `typestr` make the component itself `dyn`, and every type string
spells it so. `show({"a": 1}); show({});`, `show({})` alone, `total([])`
and a foreach over `[]` / `{}` all compile and run. Tests: three `ti:`
`-rt` cases (the types, iteration in every caller, reading an element
nothing fills) and a `repl:` case (a committed empty array filled by a
later input, an error message).

## 10. `deepclone` and shared instances

`deepclone` copies an object that appears at two places twice, unless
the two places are on a cycle:

```
class C { int v; }
var c = C(1);
var orig = [c, c];          # two references to one instance
var copy = deepclone(orig);
copy[0].v = 9;
print(copy[1].v);           # MyLang: 1 (two copies); Python deepcopy: 9
```

A cycle IS preserved (`a.next = a; b = deepclone(a)`: `b.next` is `b`),
so the rule depends on whether the sharing happens to be circular. The
README states it ("`deepclone` does not preserve sharing").

- **A.** Keep it.
- **B.** Preserve all sharing, like Python's `deepcopy`: each object is
  copied once and every reference to it points at that copy
  (recommended; the cycle handling already keeps a map of copied
  objects).

Decision (maintainer, 2026-10-25): **B**, "it makes more sense". Done the
same day: the deep copies' cycle stack became a memo of every container
copied (`CycCopyStack`), flat arrays and boxes included, keyed by an
array's identity (its storage and slice group, so two slice expressions
over one window stay two arrays). `deepclone([inner, inner])` is one
array in the copy; a struct value's copies stay independent; the
read-only snapshot a constant or a dict key takes keeps sharing too.
Tests: `tests/functional/103_deepclone_sharing.my` and an `-rt` case;
the three tests that pinned the old rule (`83_cyclic_values.my`, the
`cycle: deepclone` case, `tests/int/40_box_kinds.my`) now assert the new
one.

## 11. The value of a multi-assignment

`w = a, b = [5, 6]` made `w` none, and so did `return a, b = ...`,
`var w = a, b = ...`; `a, b = c, d = [1, 2]` made `a` and `b` none, and
`w[0] = a, b = ...` failed only at run time. Giving it a value - the
maintainer's original idea, a list whose elements are the targets - is
`plans/multi-assign-value.md`.

Decision (maintainer, 2026-10-26): keep it valueless while a value is
hard to give it, and refuse at compile time every use of the value,
which could only ever be `none`. Done the same day: the parser refuses a
multi-assignment as an assignment's or a declaration's value, a `return`
or a `throw` (`pRefuseMultiAssignValue`); as a statement - a block's, a
`for` init or step - it is unchanged. A value would need four more
decisions first (the plan's "questions to settle"): what a compound one
yields, a spread, `_`, and its type.

## 12. Exception chaining

An exception raised while another is being handled replaces it: the
first is lost, from the program and from the uncaught-exception report.
`plans/language-deferred.md` has the request (2026-07-09) and its three
prerequisites.

Decision (maintainer, 2026-10-26): **implicit**, like Python: no new
syntax, the new exception remembers the one being handled.

