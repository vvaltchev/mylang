# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository.

> **READ `README.md` IN FULL BEFORE TOUCHING ANYTHING.** This is a small project
> and the README is
> the complete language specification (every keyword, every builtin, every
> semantic rule, with
> examples). It is not optional reference material to consult on demand — read
> the whole thing up
> front, once, so you know the language the interpreter implements. This
> CLAUDE.md covers the *C++
> implementation*; the README covers the *language*. You need both in your head
> before making changes.

> **KEEP THE DOCS IN SYNC WITH THE SOURCE — IN THE SAME CHANGE.** `README.md`
> and this `CLAUDE.md`
> are part of the codebase, not afterthoughts. Any code change must carry its
> documentation update in
> the *same commit*, never as a follow-up:
> - **`README.md`** whenever script-visible behavior changes — a
>   new/removed/renamed keyword, builtin,
>   operator, or numeric constant; changed semantics; new error conditions.
>   README is the language
>   spec; if it and the interpreter disagree, that's a bug.
> - **`CLAUDE.md`** whenever the *implementation* shape changes — a new source
>   file or `.cpp.h`, a new
>   `TypeE`/AST node, a changed design rule or invariant, a new convention, or
>   anything that would make
>   a sentence in this file wrong. After editing code, reread the relevant
>   CLAUDE.md section and fix any
>   statement the change just falsified.
>
> A change that alters behavior or architecture but leaves the docs stale is
> incomplete.

## What this is

MyLang is an educational, dynamically-typed scripting language (C-looking
syntax, Python-ish
semantics) implemented as a tree-walking interpreter in portable C++17. It has
**no dependencies**
beyond the standard library, including for its tests. The single `mylang`
executable both compiles
(lex + parse + const-fold) and runs scripts. Author's goal was to have fun
writing a recursive-descent
parser; correctness and clarity matter more than raw speed, though performance
shaped several core
design choices (see the value model below).

## Build & run

```
make -j                    # release build (-O3) -> build/mylang
make -j TESTS=1 OPT=0      # debug build, unit tests compiled in (for -rt)
make -j BUILD_DIR=other    # out-of-tree build
make clean
```

`OPT` defaults to 1 (`-O3`); `OPT=0` drops it. `TESTS=1` adds `-DTESTS`, which
is what compiles the
`-rt` suite into the binary. Base flags:
`-std=c++17 -ggdb -Wall -Wextra -Wno-unused-parameter
-fwrapv`. The Makefile auto-generates header dependencies under `.d/`.

**LTO is on by default for optimized builds.** `LTO` defaults to `OPT`, so a
release build links with `-flto=auto` (added to `BASE_FLAGS`, which the link
line passes too) — ~7% smaller binary and ~8-9% faster on `bench/`. It works on
both GCC and clang and is verified to keep `-rt` green. Build with `LTO=0` to
disable (e.g. for a faster/debuggable link); an `OPT=0` build is non-LTO anyway.

**Sanitizers default on for debug builds.** `ASAN` and `UBSAN` (AddressSanitizer
/ UndefinedBehaviorSanitizer) both default to **on when `OPT=0`** and **off when
`OPT=1`**, and either can be forced: `make ASAN=0` (debug, no ASan),
`make OPT=1 UBSAN=1` (sanitized release). The flags go into `BASE_FLAGS` so they
reach the compile and link lines. UBSan is configured `-fno-sanitize=signed-
integer-overflow`, because the codebase relies on `-fwrapv` wraparound (that
overflow is *defined* here, not a bug); it also runs with
`-fno-sanitize-recover=undefined`, so a UBSan finding **aborts** (non-zero exit)
instead of diagnose-and-continue — otherwise a real UB could print and still
exit 0 past CI's exit-code check. `-fno-omit-frame-pointer` is added whenever
either sanitizer is on. `-rt` is verified green under both.

**Assertions: `ASSERTS` (default 1).** The C `assert()` + the project's
`ML_CHECK()` invariant net (see *Invariants & hazards*) are **on for every build
type** (debug AND release), so every build and CI lane exercises them. With
`ASSERTS` on the build also enables libstdc++ container hardening
(`-D_GLIBCXX_ASSERTIONS`, ABI-safe; the libc++ analog is set per-OS in CI).
`make ASSERTS=0` defines `-DNDEBUG`, compiling all of that away — use it on an
optimized build to measure the assertion overhead, e.g. `make OPT=1 ASSERTS=0`
vs the default `make OPT=1`. **`RECYCLE` (default 0):** `make RECYCLE=1 TESTS=1`
builds the adversarial node allocator (see *Invariants & hazards*).

CMake is also supported (used by CI):
`cmake -DTESTS=1 -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .`
(`-DGCOV=ON` for a coverage build, GCC only). It enables LTO via
`INTERPROCEDURAL_OPTIMIZATION` for the `Release`/`RelWithDebInfo` configs (so
Debug and coverage builds are untouched), portable across GCC/clang/MSVC;
configure with `-DLTO=OFF` to disable. The same `ASAN`/`UBSAN` options exist
(`-DASAN=ON/OFF`), defaulting **on for a `Debug` build** and off otherwise — the
analog of `OPT=0` vs `OPT=1` — and are applied on GCC/clang only (skipped on
MSVC). CMake now also passes `-fwrapv` (non-MSVC), matching the Makefile so the
relied-upon signed wraparound is defined in CMake builds too. It also has the
**`-DASSERTS=ON/OFF`** (default ON; OFF defines `NDEBUG`, and since CMake's
optimized configs add `-DNDEBUG` themselves, ASSERTS=ON appends `-UNDEBUG` to
keep asserts in Release) and **`-DRECYCLE=ON/OFF`** (default OFF) options,
mirroring the Makefile. CI (`.github/workflows/`) builds Debug+Release ×
g+++clang on Linux (plus a `RECYCLE=ON` lane), macOS (with libc++ hardening),
and Windows, and runs `./mylang -rt` — correctness only, no timing, so the
lanes carry as many checks as possible.

Running scripts:
```
./build/mylang FILE              # run a script; extra args become argv
./build/mylang -e 'EXPR'         # run inline source (-e args concatenated)
./build/mylang -s FILE           # dump syntax tree (const-folding) then run
./build/mylang -t FILE           # dump tokens
./build/mylang -nc FILE          # disable const-eval (compare -s with/without)
./build/mylang -ni FILE          # disable function inlining (debug)
./build/mylang -it N FILE        # inline threshold: max inlined body (nodes)
./build/mylang -nr FILE          # parse/validate only, don't run
./build/mylang -nti FILE         # disable static type inference / checking
./build/mylang --debug-ti FILE   # dump every identifier's inferred type + uses
./build/mylang -a FILE           # analyze: source colored by optimization
./build/mylang -a --no-color F   # same, plain (for piping / diffing)
./build/mylang -T CATS FILE      # trace the compiler's reasoning to stderr
./build/mylang --trace all FILE  # CATS: infer,inline,specialize,template,
                                 # autoconst,autopure,arrays,fold, or all
```
`-T`/`--trace` enables diagnostic trace categories (see `trace.{h,cpp}` and the
REPL `:trace`) BEFORE compilation, so a script run narrates each optimizer
decision to stderr (colored on a stderr TTY). The same categories drive the
`trace()`/`traceoff()`/`tracing()` builtins and the REPL `:trace [<cat>...]
on|off`. OFF by default — zero cost on a normal run (one mask test per guarded
site).
`--debug-ti` runs inference (non-strict) and prints one tab-separated `ti`
record per declared identifier — `name, kind (var|const|param|func), line, col,
const, type, uses(line:col,...)` — then exits without running. It is the audit
tool for the mandatory-`dyn` / type-driven work (see
`plans/type-driven-specialization.md`): used to find identifiers inferred `dyn`
/ `array<dyn>` and decide whether each is justified (annotate with `dyn`) or an
inference gap (fix the inferencer).

`-a` / `--analyze` reprints the source **verbatim** with ANSI colors marking
where each compile-time optimization fired (a legend header is printed; exits
without running; `--no-color` for plain). It is **non-strict** (analyzes code
that a normal run would reject for a bare `dyn`). The legend: **yellow** =
became const-like automatically (an auto-const `var` at decl + folded uses, an
un-mutated parameter, an auto-`pure` function); **green** = a flat (unboxed)
`array<int>`/`array<float>`; **red** = an `array<dyn>`; **blue** = an inlined
call (expression-body or tail); **cyan** = a call redirected to a `name$sN`
specialization clone; **magenta** = a call folded to a literal at compile time
(pure/auto-pure/const-builtin with const args); **dim** = dead code the
optimizer eliminated (a const-condition branch / `while (false)`). Precedence:
call-site (blue/cyan/magenta) > array storage (green/red) > yellow; a dead range
dims regardless. Implementation: a `Loc`-keyed `AnalysisInfo` (`analyzer.h`)
populated by the passes only when threaded in — array colors from a non-strict
inference pass (`collect_array_analysis`), auto-pure/param from a post-resolve
walk (`collect_resolver_analysis`), and the mutation-time decisions (auto-const
vars, dead code, inlined/specialized/folded) recorded *as they happen* by the
parser (`ParseContext::analysis`), AutoConst, and the Inliner; `mylang.cpp`
renders. Like `-s`/`--debug-ti`, it is a CLI inspection tool, not unit-tested in
`-rt`.
`-s` / `-nc` are the two indispensable debugging tools: `-s` shows you exactly
what survived
const-folding, and `-nc` lets you see the tree as written before folding. Reach
for them whenever
behavior surprises you.

## Tests

```
./build/mylang -rt               # run the whole suite (needs a TESTS=1 build)
./build/mylang -rt -s            # same, but dump the tree of a failing test
```

Tests are **not** a separate framework — they are entries in the `tests` table
(a `static const std::vector<test>`) in `src/tests.cpp`. Each entry is a tuple:
a name, a list of source-line strings, and an optional `&typeid(ExpectedEx)`.
`check()` lexes+parses+evals the joined source lines. A test **passes** if it
throws nothing — or, when an expected exception type is given, throws *exactly*
that type (compared via `&typeid(e) != t.ex`). There is no single-test CLI
selector; `-rt` runs all of them and `exit(1)`s if any fail. Add a test by
appending an entry to that table (no registration needed). Note the expected
exception is matched against the *static* C++ type; user-level `throw ex("Foo")`
always surfaces as `ExceptionObject` (a.k.a. `DynamicExceptionEx`), not a
distinct C++ type.

## Benchmarks

`bench/` is a standalone performance suite comparing MyLang against CPython,
construct by construct
(`bench/my/NN_name.my` paired with `bench/py/NN_name.py`; a few MyLang-only
features like const-folding
have no `.py`). MyLang scripts use the `.my` extension. `python3 bench/run.py`
times every pair (best of N), prints a
`my/py` ratio table (the ratio number is colored on a TTY — a green→red
gradient, plain when piped or `--csv`), and
checks the two implementations printed matching results. Each script takes a
`scale` multiplier as its
first argv. It is *not* wired into `make`/CI and has no third-party deps.
`bench/README.md` is also the
written answer to "do MyLang and Python behave the same?": they do *observably*
— assignment aliases,
slices act like independent copies (MyLang via lazy copy-on-write, so read-only
slicing is far cheaper),
`clone()` makes a shallow copy and `deepclone()` a deep one — with the
divergences (64-bit wrapping vs bignum,
truncating vs flooring division, unordered vs insertion-ordered dicts)
enumerated there. (Floats match: both are 64-bit IEEE `double`.)
`bench/verify_semantics.{my,py}` assert that equivalence and must both print the
same line.

## Source layout & compilation model

**Only `src/*.cpp` are compiled** (the Makefile globs them) — eighteen
translation units:
`lexer.cpp`, `parser.cpp`, `syntax.cpp`, `resolver.cpp`, `inferencer.cpp`,
`eval.cpp`, `types.cpp`, `stype.cpp`, `trace.cpp`, `coderender.cpp`,
`backtrace.cpp`, `errfmt.cpp`, `highlight.cpp`, `lineedit.cpp`, `replhelp.cpp`,
`repl.cpp`, `mylang.cpp`, `tests.cpp` (the last six are the REPL — see "The
interactive REPL" below; `trace.cpp` is the diagnostic tracer and
`coderender.cpp` the optimized-AST "decompiler", both used by the REPL).

- `mylang.cpp` — CLI entry point, arg parsing, the top-level `try/catch` that
  turns thrown
  `Exception`s into formatted error output (`dumpLocInError` prints the source
  line + a `^` caret).
- `lexer.cpp` / `lexer.h` — `lexer(line, line_no, tokens)` appends tokens for
  one source line. The
  CLI feeds it line by line so token `Loc`s carry real line/column numbers.
- `parser.cpp` / `parser.h` — recursive-descent parser, const-folding woven in.
  `pBlock()` is the
  entry point.
- `syntax.h` / `syntax.cpp` — the `Construct` AST node hierarchy, its
  `serialize()` (what `-s` prints), and `clone()` (a deep copy of a subtree,
  used by inlining; pure virtual so every concrete node must provide one — see
  the `clone_as`/`copy_base_fields`/`clone_ops_into`/`clone_elems_into`
  helpers). Also `desugar_named_call` (over a `ParamSpec` view) — the shared
  named-argument → positional rewrite used by both the parser and the
  inferencer (see *Static type inference*).
- `resolver.cpp` / `resolver.h` — `resolve_names(root)`, a post-parse pass that
  assigns function
  params slot indices for O(1) access at runtime (see the value model section).
  Optional and always
  safe: anything it leaves unresolved falls back to the runtime map lookup. The
  same file also hosts the **auto-const** folder (the `AutoConst` class), run at
  the end of `resolve_names` (see the const-evaluation section), and the
  **inliner** (the `Inliner` class), run after it (gated by `-ni`; see the value
  model section / `plans/function-inlining.md`).
- `eval.cpp` — the `do_eval()` bodies: the actual tree-walking interpreter.
- `types.cpp` — the single TU that stitches the type system and builtins
  together (see next section).
- `stype.cpp` / `stype.h` — the **static-type lattice** for type inference
  (`STy`/`STyArena`: `resolve`/`unify`/`assignable`/`join`/`equal`/
  `to_string`). Distinct from the runtime `Type *` ops table — this is what the
  compile-time inferencer reasons over (type variables, nullability `opt`,
  structural array/dict/func shapes). See `plans/type-inference.md`.
- `inferencer.cpp` / `inferencer.h` — `infer_types(root)`, the **whole-program
  static type inference + checking** pass (see the dedicated section below).
- `backtrace.cpp` / `backtrace.h` — `format_backtrace()`, which renders an
  `Exception`'s captured call-stack (see the error model section).
- `analyzer.h` — header-only (no TU): the `AnalysisInfo` `Loc`-keyed annotation
  collector + `AnnoKind` for the `-a`/`--analyze` colored optimization view. The
  collectors live in the relevant passes (`collect_array_analysis` in
  `inferencer.cpp`, `collect_resolver_analysis` in `resolver.cpp`, mutation-time
  records in `parser.cpp`/`resolver.cpp`); the renderer is in `mylang.cpp` (see
  the `-a` description under "Build & run").

**The `.cpp.h` convention.** Files under `src/types/` and `src/builtins/` are
named `*.cpp.h` and are
`#include`d *once* into `types.cpp`. Each starts with a comment: "this is NOT a
header file… it's a
C++ file in the form of a header, just because it's faster to compile it this
way." So `types.cpp` is
the one TU that compiles every `TypeXxx` class and every `builtin_xxx`. When
adding type or builtin
code, put it in the matching `.cpp.h` and rely on it being pulled into
`types.cpp` — it will not
compile standalone, and there is nothing to add to the Makefile.
`builtins/reflect.cpp.h` holds the **runtime reflection** builtins
(`globals`/`typeof`/`signature`/`layout`/`specializations`) and the shared
`reflect_*` rendering helpers (signature/type/layout strings) the REPL's
introspection commands reuse; it is `#include`d last (after the other builtins)
so it can call `arr_elem_at`. See `plans/repl-introspection.md`.

**Why so many headers are templates.** `type.h` (`TypeTemplate`),
`sharedarray.h`
(`SharedArrayObjTempl`), `shareddict.h`, `exceptionobj.h` are templated *not*
for genericity but to
break a circular include order: they need `EvalValue`/`LValue`, which need them.
`evalvalue.h`
instantiates them with concrete types via typedefs
(`typedef TypeTemplate<EvalValue> Type;`,
`typedef SharedArrayObjTempl<LValue> SharedArrayObj;`, …). Treat these typedefs
as the real types.

## The pipeline

**lexer → recursive-descent parser (with const-folding woven in) →
type-inference + checking → name-resolution pass → typed specialization →
tree-walking evaluator.**

Type inference runs *between* parsing and `resolve_names` (on the clean,
un-inlined tree); the typed-node *specialization* it enables runs *after*
`resolve_names`. Both are gated by the CLI's `-nti` and on by default. See
"Static type inference" below.

**The post-inference optimizer pipeline is one shared function,
`run_optimizers` (`resolver.cpp`/`.h`): `resolve_names` (slotting + auto-const
+ inlining + specialization) then `specialize_types` (M8).** Both drivers call
it — the script path (`mylang.cpp`) with `repl_mode=false`, the REPL
(`repl.cpp` `do_eval`) with `repl_mode=true` + the prior-input scope — so the
REPL transforms the tree EXACTLY like a script (only the inference *front* end
differs: one-shot `infer_types` vs incremental `ReplInfer::check_input`, both
built on the same `Inferencer::infer_one`). A new pass added to
`run_optimizers` reaches both identically — the REPL's optimization parity is
not maintained by hand. Likewise the `-a`/`:analyze` collect-and-render
pipeline is one shared `analyze_and_render` (`analyzer.cpp`).

### Lexer

Produces a `vector<Tok>`. A `Tok` is `{ TokType, Loc, value/op/kw }` where
`TokType` ∈ {integer, id,
op, kw, str, floatnum, unknown}. Operators (`Op` enum, `operators.h`) and
keywords (`Keyword` enum,
`lexer.h`) are recognized via `std::map`s built once from the `OpString` /
`KwString` arrays — keep
those arrays index-aligned with the enums if you add tokens. `invalid_tok` is
the EOF sentinel.
Chars are 8-bit; **no Unicode** (deliberate, to stay small). A **trailing `!`**
is part of an identifier (Ruby/Scheme "bang" convention, e.g. `get!`), but only
when it is not the start of `!=` — so `x!=y` still lexes as `x != y`.

### Parser — operator-precedence ladder

Binary-operator precedence is encoded as a ladder of parse functions
`pExpr01 … pExpr14`, each
producing a matching AST node class `Expr01 … Expr14`. **The numbering has
gaps** — only
`02,03,04,05,06,07,08,09,10,11,12,14` carry real operators (the level numbers
match C precedence; an unused level is skipped):

- `pExpr01` — primaries + postfix chains: literals, `()`, `[...]` array, `{...}`
  dict, identifiers, then any run of call `(...)`, subscript/slice `[...]`,
  member `.id`. A call's argument list is parsed by `pArgList` (not the generic
  `pList`), which also accepts **named arguments** `name: value` (label = a bare
  IDENT followed by `:`, one-token lookahead) — see the inferencer's
  `lower_named_args` and README *Named arguments*. A trailing **`++`/`--`**
  (`Op::inc`/`Op::dec`) after the postfix chain makes a **postfix**
  `IncDecExpr`.
- `pExpr02` — unary `+ - ! ~` (right-recursive, so `!!x`, `-+x` work); `~` is
  bitwise NOT here (the same `Op::bnot` token is the `dyn` alias in a *param*
  position, but that is handled in `pFuncParam` before any expression, so they
  never collide). A leading **`++`/`--`** makes a **prefix** `IncDecExpr` (so
  `--1` now lexes as decrement-of-`1`, a compile error like C — not `-(-1)`)
- `pExpr03` — `* / %`
- `pExpr04` — `+ -`
- `pExpr05` — `<< >> >>>` (shift; `>>` signed/arithmetic, `>>>` unsigned/logical)
- `pExpr06` — `< > <= >=`
- `pExpr07` — `== !=`
- `pExpr08` — `&` (bitwise AND), `pExpr09` — `^` (XOR), `pExpr10` — `|` (OR)
- `pExpr11` — `&&`
- `pExpr12` — `||`
- `pExpr14` — assignment `=  +=  -=  *=  /=  %=`, plus `var`/`const` decls and
  id-list targets

`pExprGeneric<ExprT>` implements the common left-associative chain: it collects
`(Op, operand)` pairs
into a `MultiOpConstruct`, evaluated left-to-right in `do_eval` by mutating an
accumulator
(`val.get_type()->add(val, rhs)`, etc.). `pBlock` is the entry point; `pStmt`
dispatches statements
(`if`/`while`/`for`/`foreach`/`func`/`try`/`throw`/`return`/braced
block/expression-statement).

**Bitwise / shift operators (`~ & ^ | << >> >>>`) are int-only.** New `Type`
virtuals `band`/`bor`/`bxor`/`shl`/`shr`/`ushr` (binary) and `bnot` (unary) —
base `Type` throws `TypeErrorEx`, only `TypeInt` implements them, so a `float`
operand (which `num_bin_op` routes to `TypeFloat`) raises a type error; a `bool`
promotes to `int` first like the other numeric ops, and the result is always
`int`. `>>` is a SIGNED (arithmetic, sign-extending) right shift, `>>>` the
UNSIGNED (logical, zero-filling) one (JavaScript semantics). The shift bounds /
sign handling live in **`bitops.h`** (`bit_shl`/`bit_shr`/`bit_ushr`: count must
be `>= 0` or `InvalidValueEx`, a count `>= 64` saturates to `0` / sign-fill
instead of UB) — shared by `TypeInt` AND the **M8** unboxed path so they compute
identically. M8: the BINARY bitwise nodes (`Expr05`/`08`/`09`/`10`) specialize
into a `TypedScalarExpr` (`Cat::arith`, the int `eval_int` loop, which gained
the `band`/.../`ushr` cases) — so a bit-manip tight loop is unboxed (~2x over
`-nti`); unary `~` (an `Expr02`) stays the boxed path. The inferencer's
`binop_result`/`unary_result` type them (int, int/bool operands; a float is
`dyn` → the check pass reports "operator does not apply"). Precedence matches C
exactly (see the `pExpr0N` ladder above), including the `a & b == c` →
`a & (b == c)` trap.

A `fl` bitmask of `pFlags` (`pInDecl`, `pInConstDecl`, `pInLoop`, `pInStmt`,
`pInFuncBody`,
`pInCatchBody`) is threaded through every parse function and gates legality:
`break`/`continue` only
under `pInLoop`, `return` only under `pInFuncBody`, `rethrow` only under
`pInCatchBody`, and
`var`/`const` set `pInDecl`/`pInConstDecl` so `pExpr14` knows to *declare*
rather than *assign*.

### Const-evaluation — the central design feature

Constants are evaluated **at parse time**, like C++ `constexpr`. This is the
project's defining trait
and it lives *inside the parser*. Mechanics:

- `ParseContext` owns a chain of **const `EvalContext`s** (`const_ctx`).
  `pBlock` pushes a fresh
  nested const ctx on entry and pops it on exit, so the const scope chain
  mirrors lexical scope.
- Every `Construct` carries an `is_const` flag, propagated bottom-up: a node is
  const iff all its
  children are const. String literals are const; `var`/freshly-declared
  identifiers are not.
- As soon as the parser finishes a const node, it evaluates it against
  `const_ctx` and calls
  **`MakeConstructFromConstVal()`** to replace the subtree with a literal node.
  That function inlines
  `int`/`float`/`none`/`str` unconditionally, and `arr`/`dict` only when
  `process_arrays` is set — in which case it bakes the whole value into **one
  `LiteralObj` node** (`syntax.h`), not one literal per element. (It stores
  `v.clone()` so a small slice of a huge const array doesn't pin the huge
  buffer.) `LiteralObj` carries an **`immutable`** flag. The materializer sets
  it when **either** the target is a `const` decl (`fl & pInConstDecl`) **or the
  value itself is already read-only** (`is_readonly_value()`). The second case
  is how **const-ness propagates**: a slice/element/result derived from a const
  is read-only, so `var s = y[1:3]` (with `y` const) keeps `s` read-only —
  `var` only makes the *name* rebindable, not the value mutable. (This mirrors
  runtime, where the value carries the flag; a *fresh* literal isn't read-only,
  so `var a = [1,2,3]` stays mutable.) `LiteralObj::do_eval` (`eval.cpp`) then
  either:
  - for an `immutable` result: hands out the **deep read-only** value (baked via
    `make_const_clone()` — every array/dict in it, recursively, is flagged
    read-only — and shared, since it can't be mutated), so it is immutable
    through *any* alias (e.g. a non-const function parameter, or a `var`); or
  - for a mutable result: a *fresh, fully-mutable deep copy* via
    `make_mutable_clone()`, exactly what the old per-element
    `LiteralArray`/`LiteralDict` produced at runtime — a `var` bound to it must
    be writable, and re-evaluating the node (loop body, function called twice)
    must not see a prior mutation.

  Direct reads of a const symbol (`y[k]`, `len(y)`) still fold to literals at
  parse time; the runtime read-only flag is what additionally enforces
  immutability through aliasing (see the copy-on-write section). `LiteralObj` is
  `is_const` but deliberately **not** a `Literal` (which in this codebase means
  a *scalar* literal — see auto-const's `is_scalar_literal`), so an array/dict
  value is never mistaken for a promotable scalar.
- **Scalars vs. containers (`ShouldConstSymbolExistAtRuntime`).** Const scalars
  are inlined
  everywhere and their declaration is dropped from the runtime AST entirely (the
  decl parses to a
  `NopConstruct`, which `pBlock` discards) — `-s` shows them simply gone. Const
  **arrays, dicts, and
  funcs** are *kept* as real runtime symbols (declared once), because inlining a
  million-element array
  at every use site would be wasteful; operations on them like `arr[2]` or
  `len(arr)` still get
  const-folded to literals though.
- **Const-expression de-duplication (CSE).** The three sites that bake an
  array/dict (`pAcceptCallExpr`, `pAcceptSubscript`, the `pExpr14` decl-rvalue)
  go through **`cse_materialize()`** (`parser.cpp`) instead of calling
  `MakeConstructFromConstVal` directly. It builds a canonical string key for the
  expression (`cse_key`/`cse_key_rec`: identifiers resolved to their const
  `LValue *` so shadowing can't alias; only cheap leaves — ids and scalar
  literals — are eval'd, structural nodes recurse without evaluating; a
  `CSE_KEY_CAP`-byte cap bounds key cost and skips huge literals) and looks it
  up in **`CseCache`** (`parser.cpp`), a stack of `unordered_map<string,
  EvalValue>`
  scopes pushed/popped by `pBlock` in lockstep with `const_ctx`. On a hit it
  shares the already-baked **deep read-only** value (no re-eval, no re-clone) —
  this is why two identical const exprs report equal `intptr()`. On a miss it
  bakes via `make_const_clone` and caches the value *only when it is read-only*
  (the sole safely-shareable case; mutable `var`-bound literals are never
  cached/shared). Popping a scope with its block is what stops a freed block's
  reused stack addresses from colliding with a live key. `pExpr14` skips
  re-materializing an rvalue that is *already* a `LiteralObj` (a subscript/call
  result baked at its own site), so the de-dup lives at one layer and no double
  clone happens. CSE is a pure optimization (miss == old behavior); its win is
  compile time + memory, not runtime speed (folding already makes each use a
  literal). `bench/52_cse_dedup` and the `CSE:` tests cover it.
  `cse_materialize` is PIMPL-friendly: `CseCache` is forward-declared in
  `parser.h` with an out-of-line `~ParseContext()`.
- **Statement folding:** an `if` with a const condition is replaced by just its
  taken branch; a
  `while`/`foreach` proven to never execute (const-false condition / const-empty
  container) is dropped.
- **`pure func`s are the escape hatch.** `pure func f(...)` parses with
  `is_const = true` and is
  registered into `const_ctx`, so it *can* be invoked during const-eval (a plain
  `func` cannot). Pure
  funcs may not have a capture list and see only consts + their own params. A
  `CallExpr` folds when
  the callee and all args are const — that's how
  `sort(arr, pure func(a,b) => a<b)` runs at parse time.
- **Early failure:** exceptions raised *during* const-eval propagate immediately
  and are *not*
  catchable by script `try/catch` (the parser never enters a const assignment
  inside a try). This is
  intentional — const errors should fail the build, not be swallowed.
- A const decl in `pInConstDecl` whose rvalue isn't actually const throws
  `ExpressionIsNotConstEx`.

`-s` (with vs. without `-nc`) is the way to *see* all of the above happening.

**Auto-const (`AutoConst` in `resolver.cpp`).** A post-parse folding pass that
does for plain `var`s what the parser does for `const`. It runs at the end of
`resolve_names()` (so it can use the resolver's per-slot write counts and
slot identity). For each function (and the top-level "main"), it:
- **promotes** a `var` that is *write-once* (`slot_writes[slot] == 1`, i.e. only
  the declaration writes it) with a constant **scalar** initializer into a
  compile-time constant (keyed by slot), drops the declaration, and folds every
  use to the literal — cascading in declaration order, so a `var` derived from
  earlier auto-consts also promotes. Uses are folded in every read position,
  including a `return` expression (`fold_child` handles `ReturnStmt` explicitly:
  it's a plain `Construct`, not a `SingleChildConstruct`, so `fold_reads` skips
  it — without that a promoted `var` used only in a `return` would
  have its decl dropped but the use left dangling as an undefined variable);
- **folds** all-literal arithmetic/logic/comparison (`MultiOpConstruct`) to a
  single literal, reusing the interpreter (`mo->eval(&cctx)` against a const
  `EvalContext`);
- **short-circuit / identity folds** a logical op with const LEADING operands.
  mylang's `&&`/`||` yield a **bool** (not the operand, unlike Python — `false
  || 5` is `true`, not `5`), which shapes the rules. A const that **determines**
  the result — `false && rest` → `false`, `true || rest` → `true` — folds the
  whole expression to that bool (sound regardless of `rest`: it is
  short-circuited, so never evaluated, even side effects / an undefined name).
  This is what makes a feature-flag guard fold: `const DEBUG = false; if (DEBUG
  && heavy())` → `if (false)`, which the DCE then drops — matching C++ `-O3`. A
  **non-determining** leading const (`true && rest`, `false || rest`)
  contributes nothing and is dropped: if ≥2 operands remain it stays a logical
  op (`false || a || b` → `a || b`, sound for any types); if exactly ONE remains
  the result is `bool(operand)`, so the const is dropped **only when that
  operand is already bool** (`false || (x>0)` → `x>0`, via `produces_bool`: a
  comparison / `&&` / `||` / `!` / bool literal, or its M8 typed form) — `false
  || x` for a plain int `x` is left alone, since `bool(x) ≠ x`.
- performs **dead-code elimination**: an `if`/`while` whose condition folds to a
  literal has its dead branch dropped (`while (false)` removed). Crucially, a
  branch it proves dead is *eliminated, not folded* — auto-const only analyzes
  code it proves reachable (this is its DCE; eager fail-on-error in dead code is
  the *parser's* behavior for explicit `const`/literals, not auto-const's).
- **Safety (`prescan_blocked`)**: a slot is *not* promoted if the variable is
  captured by a nested function (the capture must stay an identifier), passed as
  the **first arg** of a builtin that takes it as an lvalue/identifier
  (`append`/`push`/`pop`/`insert`/`erase`/`intptr`/`undef`, listed in
  `is_lvalue_arg_builtin` — a literal there throws `NotLValueEx` or breaks
  `undef`), used as a subscript/member base (`a[i]`, `a.k`), or is a `foreach`
  loop variable (implicitly reassigned each iteration despite its write count).
  Args to pure/user functions and read-only builtins are **not** blocked, so
  they fold — this is what lets pure-call folding and `isconst()` work.
- **Same early-failure rule as the parser:** a fully-constant expression in
  *reachable* code that throws when evaluated (e.g. `6/0`, a type mismatch) is
  **not** deferred to runtime — the exception propagates out of `resolve_names`
  and aborts before execution. `try/catch` does not catch it. The `runtime()`
  builtin is the documented opt-out: it is a non-const builtin, so it is not in
  the folder's const context and a call to it never folds; the containing
  expression stays a runtime computation (its *argument* is still folded, so
  `runtime(1/0)` still fails at compile time).
- **Pure-call folding.** `register_pure_funcs` first registers every
  effectively-pure NAMED function (`FuncDeclStmt::effective_pure`) into the
  folder's const `EvalContext` (`cctx`), which already holds the const builtins.
  Then a `CallExpr` with all-const arguments folds by simply `eval`-ing it
  against `cctx`: a pure func / const builtin runs and yields a literal;
  anything else (a non-pure func, `runtime()`, `print`, ...) is absent, the
  lookup throws `UndefinedVariableEx`, it's caught, and the call is left for
  runtime. This is how an auto-pure func's const-arg calls fold even though
  auto-pure is decided *after* parsing (explicit `pure` funcs already fold at
  parse time via the parser's const-eval).

Implementation notes: slots are never reused across sibling scopes
(`FuncState::next_slot` is monotonic), so the slot-keyed map can't collide.
The pass needs a *complete* tree traversal, but `for_each_child` deliberately
omits the nodes `walk()` handles itself (Block/for/foreach/try/`Expr14`), so
`prescan_blocked`, `register_pure_funcs` and the folders descend into those
explicitly.

**Auto-pure & const/pure introspection.** `func_body_is_pure` (`resolver.cpp`),
run after a function body is resolved, promotes a non-pure, capture-free func to
`effective_pure` when every free identifier (`sym.kind != local`) is
`is_const` (a const global/builtin/explicit-pure func) **or the name of a
function already proven pure** (`Resolver::pure_func_names`, populated in
walk order as `process_function` decides each), and it nests no function. So a
function that calls an *earlier* auto-pure helper is itself recognized pure —
`func f(x,y)=>add(x,y)` is pure once `add` is, so `f(1,2)` const-folds (the
whole pure chain folds at compile time, like `-O3`). Still conservative:
self-recursion and a call to a not-yet-decided (forward-referenced or
mutually-recursive) func stay impure. **Cross-input** (REPL): `resolve_names`'s
`prior_pure` arg (the persistent runtime scope) seeds both `pure_func_names`
(so a new input's `f` calling an earlier-input `add` is recognized pure) and
`AutoConst`'s fold context with the earlier inputs' effectively-pure FuncObjects
(so a call to one *folds* across inputs — `func f2()=>f(1,2)` with `f`'s
instance from an earlier input becomes `=> 5`). The same `prior_pure` scope is
also handed to the **Inliner** (`Inliner(.., prior_scope)`): its `run()`
registers earlier inputs' **effectively-pure** functions (and their
instances) into `funcs`/`spec_funcs`, so a call to a prior-input pure function
**inlines / specializes** across inputs too — `func caller(a,b)=>f(a,2*b)`
then (later) `func c2(x)=>caller(x,3)` folds `c2`'s instance body to `x + 6`,
matching what one compilation (or C++ `-O3`) produces. Cross-input is restricted
to **pure** functions on purpose: the inliner only ever CLONES a callee body, so
reusing a prior retained decl is safe, but an *impure* prior function reads/
writes mutable global state that may differ at the new site (and its result
isn't compile-time-known anyway), so it is left a runtime call. Only pure
*functions* are seeded (never a runtime var); registration skips any name the
current input defines, so a redefinition wins and a redirected call already
points at the current input's own instance. **A prior input's body is
POST-specialization** (it already ran `specialize_types`, so it can hold M8
`TypedScalarExpr` nodes — which the inliner never sees in a normal single
compilation, since it runs *before* `specialize_types`). So
`for_each_child`/`for_each_child_slot` (and `is_foldable_expr`) were taught the
`TypedScalarExpr` case: substitution descends into it (else a param used twice —
once outside, once inside a typed `a*2` — is only half-replaced, dangling the
inner `a`), and a const-operand one refolds to a literal (`4*2`→`8`). Without
this, `func mk(a)=>[a,a*2]` inlined cross-input crashed with "Undefined 'a'".
**AutoConst folds an
EXPRESSION-bodied function's body** (`fold_func_body` handles a bare-expr
body, not only a `{...}` block — the older `fold_function` skipped the former,
so a pure call in `func g()=>f(1,2)` never folded).
`FuncDeclStmt::{explicit_pure, effective_pure}` back the
runtime builtins `ispuredecl()`/`ispure()` (they evaluate the arg to a
`FuncObject` and read its `FuncDeclStmt`). `isconst()`/`isconstdecl()` are
resolved in the auto-const pass (`fold_isconst`): `isconstdecl` is true for
parse-time consts and `const` params; `isconst` also accepts auto-const vars and
auto-const params. All four are registered as runtime builtins (with fallback
bodies) so the names resolve even when the pass doesn't fold them.

**In progress: function inlining & specialization.** Designed and being built;
see `plans/function-inlining.md` for the full plan and task order. Three aims:
(1) *specialization* — propagate a const/auto-const argument into a (possibly
non-pure) function and fold (the missing half of const-parameter propagation,
since today only pure/auto-pure whole-call folding crosses a call); (2) *inline
trivial bodies even with no const args* (`func f(x) => x+1` called `f(y)` folds
to `y+1` in place — removes call overhead and exposes the body to the caller's
const-folder); (3) keep backtraces identical with inlining on/off. The two
formerly-open questions are settled: the **criterion** is "specialize→fold→
measure→decide" (inline if tiny-after-fold; else emit a shared specialized clone
if it folded a lot; else leave the call), and the **backtrace** uses
"inlined-at" chains (`InlineCtx`, `errors.h`) flushed by `flush_inline_frames`
(`backtrace.cpp`) at two error-path points (`Construct::eval` and
`do_func_call`'s catch), keyed off `Exception::inline_origin_emitted` (see the
re-fold paragraph), leaving `format_backtrace` unchanged. *General*
algebraic simplification of non-constant operands (`x+1-1 -> x`) is deliberately
out of scope — unsound in a dynamically-typed language without type narrowing
(float non-associativity, `+`-overloading on strings/arrays, preserved type
errors). **Status:** the `InlineCtx` backtrace foundation exists (a
`Construct::inline_ctx` field, the flush helper + the `Construct::eval` hook),
**AST deep-clone** (`Construct::clone()`, all node types), and the **size-only
inliner** (`Inliner` in `resolver.cpp`, run after `AutoConst`; gated by `-ni`).
It splices eligible direct calls — top-level, expression-bodied, non-capturing,
non-recursive, no nested function, arity match, body ≤ a node threshold (`-it
N`, default 24), sound arg use
(an arg is evaluated as often as the param is used; side-effecting args neither
dropped nor duplicated). The spliced body's params are replaced by the args
(which inherit the parameter occurrence's loc), and the whole splice is tagged
with an `InlineCtx`. **The inliner re-scans each splice** (`walk(slot, depth+1)`
after splicing), so a g-into-f-into-h chain collapses in one pass even when
declaration order defeats the bottom-up walk (h declared before the callees it
transitively reaches) or a call is newly exposed by the re-fold — this is the
"fixpoint." Two bounds keep it finite: `MAX_INLINE_DEPTH` (16) caps nesting so
mutual recursion (`a()=>b(); b()=>a()`) terminates, and `inline_budget`
(`max(4096, 8 * program nodes)`) caps total nodes added so breadth-doubling
(`f()=>g()+g()`) can't blow the tree up; hitting either bound just leaves the
remaining calls in place (still correct — they run at runtime). A re-scanned
splice's call site already carries an `InlineCtx`, so the new frame's parent is
that existing chain (not null) and `rebase` (in `tag_inline`) re-roots the
body's own chains under it — arbitrarily deep nesting renders correctly, and a
chain mixing inlined and physical/recursive frames shows them in order. The
inlined-frame flush is keyed off
`Exception::inline_origin_emitted` (a bool), **not** the loc once-guard: the
innermost inlined node's `Construct::eval` emits its frames once and sets the
flag, so an error that arrives with a loc already set (a builtin like
`append(tbl, 9)`, a not-an-lvalue assignment `d.k = v`) still keeps its frames.
`do_func_call` sets the same flag after its own call-site flush so the enclosing
`CallExpr` doesn't re-emit, while each physical call's flush stays unconditional
(multi-level inlined call sites all show). Backtraces for **body** errors are
byte-identical with/without inlining;
**known limitation** — an error *evaluating an argument* (e.g. an undefined var)
is attributed to the inlined callee rather than the call site (the arg node is
both the call-site value and the in-body operand). After splicing, the inliner
**re-folds** (`Inliner::refold`): a `MultiOpConstruct`, subscript, slice, member
access, or const-builtin call folds to a literal when its operands are
compile-time constants — scalar/array/dict literals *and* const globals (the
top-level const array/dict decls are seeded into `cctx` by
`seed_const_globals`). A `has_slotted_local` guard keeps it from dereffing a
missing frame (it never evaluates a node referencing a runtime local), and it
skips lvalue positions (an assignment target, an lvalue builtin's first arg) so
a write target is never turned into a value. So a const arg propagates into a
*non-pure* expression function (`f(3)` with `f(x) => x*10+g` -> `30+g`;
`a[0]` -> `10`, `len(a)` -> `3`, `tbl[0]` -> the element) — the half AutoConst's
whole-call folding misses. A non-inlined call to a **block-bodied** function
with const arg(s) is instead **specialized**: the body is cloned, those params
bound, and folded with DCE via `AutoConst::fold_specialized` (which *catches*
const errors and discards, so a runtime error never becomes a compile one) then
`refold`. Both **scalar** and **deep read-only array/dict** const args seed a
specialization: a read-only array/dict is sound to substitute because it is
only ever folded in read positions (`fold_reads` never rewrites an assignment
lvalue or an lvalue builtin's first arg) and any *mutation* of it throws the
same error at runtime as the un-specialized call (`prescan_blocked` gained a
`block_subscript_bases` flag; the relaxed seed set keeps the genuinely-unsafe
blocks — capture, lvalue-builtin first arg, callee, foreach var — but lets a
subscript/member READ base fold, since the
param decl is kept so an lvalue base can't dangle). The shrink decision uses
`count_all_nodes` (a *complete* traversal, unlike `node_count`, so a fold buried
in a kept `var t = a[0]+a[1]` rvalue is visible). If it shrinks, a shared clone
`name$sN` is registered (deduped by (func, const-arg tuple) — an array/dict keyed
by its `intptr` identity in `value_repr`, so the same const object shares one
clone — inserted at the root block's front) and the call redirected; the clone
keeps the same frame (no re-resolution) and a `FuncDeclStmt::display_name` makes
backtraces show the original name, not `name$sN`. A **tail call to a block-bodied
function** (`return f(args);` where f's body always returns — its last statement
is a `ReturnStmt`) is inlined *directly* (`try_inline_tail`): f's body block
replaces the return statement, sound because f's own returns become the caller's
returns and f never falls through. f's params are substituted (so they must be
non-reassigned and value-stable — `tail_arg_ok`: a caller local or a const
literal, never a global or side-effecting expr, since the body reads the param
at its use points rather than once up front), and f's locals are
**re-resolved**: a single `splice_tail` pass decides each identifier by its
ORIGINAL slot — a slot `< nparams` is a param (substitute the arg), a slot `>=
nparams` is a local (remap by `caller_fsize - nparams` into a fresh range at the
top of the caller's frame). The caller's frame (`FuncDeclStmt::frame_size`, or
the root block's `slot_count` for "main", threaded through `walk` as `fsize`)
grows by f's local count, capped at 64 (`Frame::live` is one 64-bit word); over
that, the call is left as-is. The spliced body is tagged with an `InlineCtx`
like an expression splice, so a runtime error shows f's virtual frame above the
caller's real one. Still **not done** (see `plans/function-inlining.md`
"Remaining"): the deferred type-narrowing/algebraic pass; block-tail inlining of
non-tail calls or reassigned/global args would need an args-as-locals form.

## Static type inference (`inferencer.cpp`)

A **whole-program, compile-time** pass (`infer_types(root)`) that gives every
variable, parameter, and function return a fixed static type and **rejects type
violations before the program runs**. Gated by `-nti` (default ON; also runs
under `-nr`, since type-checking is validation). It runs *after* parsing but
*before* `resolve_names`, on the clean tree (not the inlined one), and stores
nothing on the AST — it owns an `STyArena` (`stype.h`) and side tables, so it
leaves the tree untouched for the later passes (the one exception is the
named-argument desugaring below, a deliberate lowering). Full design + the
decisions behind it: `plans/type-inference.md`,
`plans/type-inference-questions.md`.

- **Named-argument desugaring (`lower_named_args`).** A call may pass arguments
  by name (`f(x: 1, z: 3)`, see README *Named arguments*). The parser attaches
  the labels to `ExprList::arg_names` (a transient `vector<const UniqueId *>`,
  parallel to `elems`, empty == all positional) via `pArgList` (replacing the
  generic `pList<ExprList>` for call args). The desugaring to positional form
  (filling a skipped *interior* optional with an explicit `LiteralNone`,
  enforcing the strict ordering — a leading run of positional args, then names
  in declaration order; reordering / duplicate / unknown name / missing-required
  is a compile error) happens in **two places**, because a named call must
  const-fold *identically* to its positional twin (the syntax cannot cost an
  optimization):
  - **At parse time** (`pTryDesugarNamedCall` → `pDesugarNamedArgs`,
    `parser.cpp`): when a named call's callee is a parse-time-const `pure func`
    (its `FuncObject` is available, giving the param `Identifier`s), the parser
    desugars *before* its const-fold step, so `const C = f(x: 1, z: 5)` folds
    just like `f(1, none, 5)`. A non-pure / forward / builtin callee can't be
    resolved here, so the parser marks the call non-const and leaves the names
    for the inferencer.
  - **In the inferencer** (`lower_named_args` → `lower_call_named_args`): right
    after the structural pass and **before** the fixpoint/check, every remaining
    named call is desugared. It resolves the callee with `callee_funcinfo` (so
    names need a directly-named function — a `dyn`/func-value/builtin callee is
    the error here), then maps labels to params (by interned name) the same way.

  After both, the tree holds only positional calls, so the fixpoint, the check
  pass, `resolve_names`, the optimizers, `specialize_types`, and eval **never
  see a name** — named args have provably zero effect on them. The inferencer
  lowering is syntactic, not type-checking, so it runs even when checks are
  disabled: `-nti` no longer makes `infer_types` a full no-op — it sets
  `checks_enabled = false`, and `run()` still does the structural pass +
  lowering, then returns before the fixpoint.

  Both sites share **one** mapping implementation, `desugar_named_call`
  (`syntax.cpp`, declared in `syntax.h`): it takes the call plus a normalized
  `vector<ParamSpec>` (`{const UniqueId *name; bool opt;}`) and owns all the
  rules (label→position by interned name, the strict ordering, the `none`
  fill, the errors). Each caller just builds the `ParamSpec` view from its own
  param representation — the parser from a `FuncObject`'s param `Identifier`s
  (`uid`/`opt_mod`), the inferencer from its `TypeSym`s (`name`/`opt_decl`) —
  and the callee-resolution + "names need a directly-named function" decision
  stays caller-specific. So a named call is rewritten, and therefore optimized,
  byte-identically wherever it is lowered. (`intern_msg`, the stable-message
  helper the compile errors need, is likewise shared from `errors.h`.)

- **Static types** are `STy` (`stype.h`), distinct from the runtime `Type *`:
  `None` (the only-none / not-yet-pinned unit), `Bool`, `Int`, `Float`, `Str`,
  `Array<elem>`, `Dict<k,v>`, `Func(params)->ret`, `Exception`, `Dyn` (explicit
  top), each with an `opt` (nullable) flag. The lattice ops are
  `assignable`/`join`/`unify`/`equal` with the numeric promotion chain
  **`Bool <= Int <= Float`** (`join` climbs by numeric rank: `join(bool,int)` is
  int, `join(int,float)` is float; `assignable` lets bool fit int/float;
  arithmetic over bools promotes to int — `binop_result`'s `arith_join` bumps a
  bool result to int, so `true+true` is int 2; comparisons/logical → `Bool`).
  `None`/`opt` give nullability; mixed container elements fall to `Dyn`; a
  scalar/kind conflict is an error.
- **Three passes**: (1) *structural* — build scopes, one `TypeSym` per
  declaration, resolve every `Identifier` to its `TypeSym`, one `FuncInfo` per
  function; (2) *fixpoint* — **Jacobi** iteration: each round recomputes every
  symbol/return type into `acc` (the `join` of all contributions) while reading
  the previous round's stable `type`, then commits; reading stable values makes
  rounds order-independent, and since kinds only climb the lattice a `join`
  conflict (e.g. `int` vs `str`) is a real, stable error raised immediately;
  (3) *check* — with final types, validate every operator, call, assignment, and
  return, throwing on a violation.
- **Inference rules of note** (the non-obvious ones): a never-(concretely-)
  called function's parameter finalizes to `Dyn` (its body must still
  type-check), while an unconstrained *local* finalizes to `None`. Param
  nullability is *declared* (`opt`/`opt dyn`) **and enforced**: the
  mandatory-`opt` rule errors if a non-opt param can actually receive `none`
  (see below) — this holds for `dyn` params too (a nullable dyn must be
  `opt dyn`; nullability is orthogonal to dyn). Local/return nullability is
  *inferred* (`None` joined with a concrete `T` is `opt T`; a `dyn` var that
  gets `none` becomes `opt dyn`). `runtime(x)` returns `Dyn`
  (its documented opt-out: it defers to runtime). `==`/`!=` are always
  well-typed (→ int); ordering is numeric-or-string. `str + anything` → str.
  Higher-order builtins (`map(func,c)`, `filter(func,c)`, `sort(c,func)`,...)
  feed the container's element type into the callback's params (named **or**
  inline lambda; `callee_funcinfo`). A `var f = <lambda>` binds `f`'s `TypeSym`
  to the lambda's `FuncInfo`, so calls to `f` type its params and check arity.
- **New surface syntax**: the `opt` and `dyn` keywords, usable as modifiers on a
  parameter (`func f(opt x, dyn y)`) or a var/const decl (`var dyn z = ...;`,
  `var opt w;`), and **combinable as `opt dyn`** (in that order) on either.
  `opt` = nullable (may hold `none`); `dyn` = dynamically *typed* (variant — any
  type; type ops are runtime-checked). **Nullability is orthogonal to `dyn`
  (Phase B):** a bare `dyn` is *non-null*, `opt dyn` is nullable — so the four
  combinations are `T` / `opt T` / `dyn` / `opt dyn`. Implemented as
  `Identifier::{opt_mod,dyn_mod}` (params, via `pFuncParam`) and
  `pFlags::{pInOptDecl,pInDynDecl}` (decls; both can be set, for `opt dyn`).
  In `STy`, `opt dyn` is `g_dyn[1]` (the opt-`Dyn` ground); `with_opt` carries
  the opt bit onto a `Dyn` kind, and `join` keeps it when a mix collapses to dyn
  (`dyn | none` → `opt dyn`).
- **Explicit type annotations** (`int x = 5;`, `func f(str s)`,
  `array a = [...]`): the primitive type keywords `bool`/`int`/`float`/`str`/
  `array`/`dict` may replace `var` on a decl/`for`-init or precede a parameter
  name, combinable as `[const] [opt] TYPE name`. They are **not lexer keywords**
  (they stay the builtins `int()`/`array()`/…); the parser disambiguates by
  one-token lookahead — a type name is a type only when the next token (after an
  optional `const`/`opt`) is the variable name (`pAcceptDeclPrefix` in
  `parser.cpp`, shared by `pStmt` and `for`-init; uses `TokenStream::peek`). The
  annotation rides on `Identifier::decl_type` (a `DeclType` enum, `syntax.h`),
  threaded from the prefix via `ParseContext::pending_decl_type` and **also
  propagated by the resolver from the declaration to every use** (so a
  reassignment can coerce). Semantics, in the inferencer (`TypeSym::ann`): a
  **scalar** annotation *pins* the symbol's type (`reset_round` seeds the
  declared `STy`, `contribute` keeps it and checks each value is `assignable` —
  so `int x = 3.5` / `int x = 5; x = 2.5` / a wrong-typed arg to a typed param
  are errors, while `float f = 3` widens); `array`/`dict` are **generic** (only
  the kind is checked, by `enforce_decl_types` in the strict block — element
  types stay inferred, so `array a = [1,2,3]` is still flat `array<int>`). A
  scalar error is gated by `strict_dyn` (off for the `-a`/`--debug-ti`
  non-strict passes). **Runtime:** `coerce_to_decl_type` (`eval.cpp`) does the
  numeric widenings (float←int/bool, int←bool) so a typed-float var/param holds
  a float — at the decl/assign (`handle_single_expr14`, `op == assign`, lvalue's
  `decl_type`) and at param bind (`bind_param`). Auto-const inlining
  (`resolver.cpp`) and the parser's const-scalar inlining both coerce too (their
  own `coerce_decl_scalar`/`check_coerce_const_scalar` copies, since a `const`
  scalar is inlined *before* the inferencer runs — that path also does the
  type-check the inferencer can't). An **uninitialized** typed decl
  (`int x;`) gets the type's zero value (`zero_value_literal`: 0/0.0/false/""/
  []/{}), or `none` when `opt`.
- **Nullable `?` suffix, `~` short form, `null` alias.** `?` is a token
  (`Op::questionmark`, `operators.h`) that is the canonical short form of `opt`:
  `int? x` ≡ `opt int x`, `var? x`, `dyn? x`, `array? a`. `pAcceptDeclPrefix` is
  a run-scanner that consumes a prefix of `{const,var,dyn,opt,?,TYPE}` ending at
  the name — `dyn` is now a standalone decl starter (`dyn z;`, not only
  `var dyn z`), and a leading `?`/`opt` alone is *not* a starter (needs
  const/var/dyn/TYPE), so `int(5)`/`x = 5` stay expressions. **Param-only short
  forms** (`pFuncParam`, rejected in body decls since they're not in
  `pAcceptDeclPrefix`): a leading **`~`** = `dyn` (reusing the otherwise-unused
  `Op::bnot` token), and a trailing **`?` on the name** = `opt` — so
  `func f(x, y?, ~z?)`. `null` is a keyword (`kw_null`) the parser treats
  identically to `none` (a `LiteralNone`). The opt flag from `?` flows through
  the existing `pInOptDecl`/`Identifier::opt_mod` path, so inference/runtime
  need no `?`-specific code.
- **Errors** are compile-time (`DECL`-style plain `Exception`s, **not**
  `RuntimeException`s, so script `try/catch` cannot catch them; `errors.h`):
  `TypeMismatchEx` (type change / bad operator / wrong arg type / not callable),
  `NullabilityEx` (`none`/`opt` used where a non-opt value is required),
  `WrongArgCountEx` (arity), `DynRequiredEx` (the mandatory-`dyn` rule below),
  `OptRequiredEx` (the mandatory-`opt` rule for params, below).
  Each carries an interned custom message + a `Loc`.
- **Mandatory `dyn`** (`enforce_concrete_decls`, ON by default via
  `infer_types(strict=true)`, off under `-nti`): a plain `var`/`const` must
  infer a *concrete* type; if its type is `dyn` it throws `DynRequiredEx`
  demanding an explicit `dyn`/`var dyn`. Phase A (`strict_deep=false`) flags a
  top-level `dyn` — `array<dyn>` is tolerated; Phase B (the flag) would recurse
  into containers. Skips params (a never-called func's param is legitimately
  `dyn`), foreach loop vars (type derived from the container), and func names.
  Runs **after** the check pass, so a var that is `dyn` *because of* a real type
  error surfaces that error first. See `plans/type-driven-specialization.md`.
- **Mandatory `opt` for params** (`enforce_nonnull_params`, same gate/timing as
  mandatory-`dyn`): a parameter that can receive `none` from *some* call path,
  if not declared `opt`, throws `OptRequiredEx` **at the param's declaration**
  ("declare it 'opt'", or **"'opt dyn'" for a `dyn` param** — Phase B: even dyn
  params are null-checked). The check pass sets `TypeSym::received_optish` when
  a possibly-none argument reaches a non-opt param (`check_call`, where the
  nullability check now runs *before* the dyn type-escape, so it applies to dyn
  too), and this rule turns that into the declaration-site error — so
  nullability is *proven* (a non-opt param, dyn or not, is guaranteed never
  `none`, body uses it without a check), not merely checked per call site. A
  call to a function *value* (no decl to point at) still reports the old
  per-call `NullabilityEx`. The nullability analogue of mandatory-`dyn`; see
  `[[nullability-opt-roadmap]]`. **A dict read is non-`opt`:** `type_of` types
  `d[k]` / `d.k` (Subscript/MemberExpr on a `Dict`) as **`V`** (the value type):
  a missing key *throws* `KeyNotFoundEx` at runtime (or returns the default of a
  default dict), so the read is a value or an exception, never `none`. The
  explicit accessors are `get(d,k)` → `opt V` (nullable lookup) and `get!(d,k)`
  → `V` (value or throw); `dict(default_value)` → `dict<dyn, typeof default>`
  (a default dict). See the dict-access runtime notes below and
  `[[nullability-opt-roadmap]]`.
- **The defer-on-Unknown/None invariant (soundness of the fixpoint).** Any type
  computation (`binop_result`, `unary_result`, `elem_of`, `type_of` of
  Subscript/Slice/Member/CallExpr-callee, `accumulate_foreach`) that meets an
  operand the fixpoint hasn't pinned yet — `Unknown` (bottom) **or** a transient
  `None` (an `array(N)` element before a write pins it) — must return
  Unknown/defer, **never `dyn`**. A premature `dyn` is sticky (it climbs the
  lattice and never comes back) and permanently poisons a self-referential
  accumulator (`acc = (acc+i)*3`, `s += sum(a)`, a foreach loop var). The check
  pass re-validates genuine errors (`require_nonopt`, not-subscriptable) with
  the final types, so deferring during accumulate hides nothing. **When
  touching the inferencer, audit any new `return A.dyn_ty()` for this.**
  `--debug-ti` dumps
  every identifier's inferred type + uses to find spurious `dyn`s. The
  invariant also applies to *contributions*, not just return types:
  `accumulate_call`'s `contribute_container` (for `append`/`push`/`insert`)
  defers when the element/key type is still `Unknown` — else it would
  contribute `array<?>`, whose **outer** kind isn't `Unknown` so `contribute`'s
  own pinned-symbol guard wouldn't catch it, tripping a PINNED global array's
  cross-input assignability check before a template-instance arg settles
  (`var g=[1,2,3]; func f(x){append(g,x);} f(3)`).
- **Finalization of unconstrained symbols.** An unconstrained *param* or
  *foreach loop var* → `dyn` (could be anything); a plain local → `none`. A func
  with an Unknown *return* → `dyn` (it returns a value that depends on
  unconstrained inputs, e.g. a func only ever passed as a value); a func with no
  value-returning path → `none` (it contributed `none` to `ret_acc`). An
  unresolved identifier / callee defers to Unknown (so the enclosing var isn't
  forced to `dyn`, and the runtime `UndefinedVariableEx` surfaces); a *builtin*
  used as a value is genuinely `dyn`.
- **Null narrowing** (`check_if`/`narrow_target`, check pass only): inside a
  proven branch a nullable var reads as non-opt — `if (x != none)` / `if (x)`
  (then), `if (x == none) ... else` (else), and the guard clause
  `if (x == none) return/throw; ...` (rest of the block). Sound (the branch
  guarantees non-none). Not flow-narrowed elsewhere.
- **Const-container types are exact** (`sty_from_value` recurses): a folded
  const array/dict is typed `array<T>`/`dict<K,V>` from its actual elements
  (heterogeneous -> `array<dyn>`; individual elements stay exact via const-fold
  of a constant-index access). Container element joins absorb `None`
  (`join_elem`) so `array(N)`-then-fill stays `array<int>`, not opt-element;
  an empty `[]` (`array<none>`) or a `dyn`-element container fits any
  `array<T>` (`sty_elem_compat` — invariance relaxed at the bottom/top element).
- **Interaction**: const scalars are already inlined to literals before this
  pass runs, so it never sees them as symbols. A statically-known type error
  that used to surface as a runtime `TypeErrorEx`/`NotCallableEx` is now a
  compile error — to keep such an error catchable at runtime, make the value
  `dyn`. **Not yet done** (deferred): function subtyping is arity-only;
  cross-statement narrowing beyond the patterns above.

### M8 — typed scalar specialization (`specialize_types`, the speed payoff)

After inference, `infer_types` stamps a `TypeHint` (`th`: `i`/`f`) on every node
it proved is a non-null int/float. **A `bool` node is stamped `i` too** — bool
flows through the int (`eval_int`) path, computing as `0`/`1`, which is exactly
its promoted value, so a typed scalar over bool operands is unboxed like int
(the boxing in `TypedScalarExpr::do_eval`/`LiteralBool` keeps a bool where it
must — a comparison/logical/`!` result, a bool literal — while arithmetic over
bools correctly yields int). `Construct`/`Identifier`/`Subscript`
`eval_int`/`eval_float` read a bool value/slot/flat element as `0`/`1`. After
`resolve_names`, `specialize_types`
(`inferencer.cpp`, called from `mylang.cpp` + the test harness, gated by `-nti`)
rewrites hot scalar nodes — `Expr03/04` (arith), `Expr06/07` (compare),
`Expr11/12` (logical), `Expr02` (unary) — over typed operands into a single
**`TypedScalarExpr`** node (`syntax.h`). It computes via **`eval_int()` /
`eval_float()`** — typed (unboxed) eval virtuals on `Construct` (default boxes
through `eval()`/`RValue`, so a typed node may call them on any child) — with no
`num_bin_op` promotion dispatch, no PMF virtual call, and no intermediate
`EvalValue` boxing. `Identifier`/`Subscript` override `eval_int`/`eval_float`
to read a resolved-local slot / an array element's scalar directly
(`EvalValue::get_ref<T>()` avoids a refcount bump); loop/if conditions take the
unboxed path via `eval_cond` when the condition is a known int (a comparison
result is bool-typed but specialized with `result_th = i`, so conditions stay
fast). The specializer
recurses bottom-up so nested typed subtrees chain `eval_int` calls with no
boxing between them. **Effect:** ~2.8x on `bench/44_primes_sqrt`, ~2x on
float-heavy reductions; the once-slower-than-Python primes benchmark is now
faster. `th` is copied by `copy_base_fields` (clones/inliner preserve it), and
the typed eval's `get<int_type>()` throws `TypeError` if inference were ever
wrong (a safety net, not silent corruption). See `plans/type-inference.md` M8.

**Counted-loop specialization (`ForRangeStmt`).** Also in `specialize_types`
(via `try_for_range`, run on the RAW `for` before its cond/inc are specialized),
the two hottest loop shapes are rewritten to a dedicated `ForRangeStmt`
(`syntax.h`):
`for (var i = start; i < bound; i += step)` and
`for (var i = start; i >= bound; i -= step)` — matched when `i` is a resolved
**int slot** (`sym.kind == local`, `th == i`), the comparison/step directions
agree (`<` with `+`, `>=` with `-`), and `bound`/`step` are **loop-immutable**:
a side-effect-free int expr (literal / slotted-local id / arith/bitwise chain —
no call/subscript/member/assignment) whose identifiers are not the loop var and
are not reassigned anywhere in the body (`fr_collect_mutated`, which reuses the
complete `Inferencer::for_each_child` so no write is missed; a local scalar can
only change via a direct assignment, and a callee can't reach the caller's
locals, so this is sound even with calls in the body). `ForRangeStmt::do_eval`
evaluates `bound`/`step` **once** (cached as raw `int_type`), then the
per-iteration condition test and increment are plain C on the slot's
`int_type` — no expression eval, no `num_bin_op`, no `TypedScalarExpr` dispatch
(the body still gets M8). `step` is null for the `i++`/`i--` form. The slot is
re-fetched each iteration so a body that reassigns `i` is honored;
break/continue/return go through the same `FlowState` as `ForStmt`. **Effect:**
~10% on the bench geomean (0.61x→0.55x). Cross-input guard: a prior REPL body
is post-specialization and may hold a `ForRangeStmt` whose `i_slot` the
inliner's substitution / tail re-resolution would not remap, so the inliner's
cross-input registration **skips** a prior function containing one
(`has_for_range`) — it still runs correctly as a call. coderender renders it as
the equivalent `for (...) /* counted */` so `:show`/`-a` make the optimization
visible. **Not yet specialized:** a `len(arr)`/call bound (excluded as a
possible side effect / mutable read), `<=`/`>` forms, and a float loop var.

### Function templates (monomorphization)

A **named** function with ≥1 *template param* — un-annotated, non-`dyn`,
non-`opt` — is a **template** (`FuncInfo::is_template`, set in
`declare_funcdecl`): not type-checked in isolation, but **instantiated per
call-site signature** as a typed clone the ordinary concrete-function path
handles. So `func f(x){var t=x+1; return t;}` never needs `var dyn t`, a
never-called template never errors, and `f(1); f("s")` makes two instances not a
type conflict. `dyn` is the explicit one-instance-any-type param. Full design +
deferrals: `plans/function-templates.md`.

**`opt`/typed params coexist with template params** and just `join` within each
clone — the signature is keyed by the *template params only*. So `func f(a, opt
b)` is a template over `a` (`b` joins; arity in `instantiate_round` is the
`[min,nparams]` range). `func f(opt x)` with no template param keeps the join
model. **A var-bound lambda** (`var id = func(x)=>x`) becomes a template iff it
is non-capturing and the var is **write-once + calls-only** (`mark_lambda_
templates`, after the structural pass, using per-symbol `writes`/`value_used`
bookkeeping — the decl write is counted in `walk_struct`, not `declare_target`,
which runs twice via hoist); a value-used / capturing / reassigned lambda stays
join. **D4:** past 64 instantiations a template's further calls run dynamically
(`tmpl_inst_count`, a one-time stderr warning).

Mechanism (`inferencer.cpp`): the fixpoint and check pass **skip** an
un-instantiated template (`accumulate`/`accumulate_call`/`check`/`check_call`
test `is_template`); an outer loop in `infer_one`, between the main fixpoint and
finalize, runs `instantiate_round` — for each template call whose arg types have
settled, it gets-or-makes the instance for that `(template, signature)`
(`make_template_clone`: a `<name>$N` id - the user name plus a per-name
monotonic counter, so it is readable AND inspectable, `typeof(f$0)`; with
`display_name` keeping the original for backtraces -
`walk_struct`'d, `is_template=false`, inserted at the root block's front),
**redirects** the call to it, and re-runs the fixpoint; the clone's params
accumulate their one signature through the concrete path. Arity is still checked
for a template call; per-arg type/nullability is checked inside each clone.
**The `(template, signature)` cache (`tmpl_cache`) is SESSION-persistent, NOT
cleared per input** — a signature already instantiated by a prior input
**reuses** that instance instead of building a duplicate (`f(2,3)` then `f(2,3)`
again in a later input both run `f$0`, not `f$0` then `f$1`). To stay
clear of the node-identity hazard below, the cache is keyed by the stable
`template_sig_key` (the template's arena-stable `FuncInfo*` + the signature's
type strings) and **valued by the instance's interned NAME** (a `UniqueId *`,
never a node pointer); the redirect resolves that name in the global scope
(`global->syms`, whose `TypeSym`/`FuncInfo` are pinned), so a prior input's
instance is reached by name, not by a stale node. `infer_input` snapshots the
cache and restores it if the input is rejected (so a rolled-back clone leaves no
entry); the clone-name counter stays monotonic. **Subtlety:** `id_sym`/
`func_of_decl` are keyed by node POINTER and
persist for the session, so a fresh input node can reuse a freed node's address
— `walk_struct` therefore **always re-resolves** an identifier (never
`if (!id_sym.count(id))`, which would keep a stale entry and bind an input's
callee to a prior clone), and `make_template_clone` clears the clone subtree's
`id_sym`/`func_of_decl`. (This was an MSVC-only, address-dependent,
non-deterministic bug, root-caused via CI instrumentation; GCC/clang +
sanitizers never reproduced it.)

## The value & type model (the subtle part)

- **`EvalValue`** (`evalvalue.h`) is a hand-rolled tagged union: a `ValueU`
  union plus a `Type *type`
  tag. It deliberately avoids `std::variant` — the comment in `flatval.h`
  records that `std::variant`
  made the whole interpreter ~50% slower on a simple loop. `size_type` is
  `uint32_t` (not `size_t`)
  specifically to keep `EvalValue` small and fast to copy.
- **`FlatVal<T>`** (`flatval.h`) is an `alignas(T) char[sizeof(T)]` buffer with
  placement-new ctors.
  It's what lets non-trivial C++ objects (`SharedStr`, `shared_ptr<…>`,
  `SharedArrayObj`) live inside
  the union. Because a union can't run their ctors/dtors, `EvalValue`'s
  copy/move/destroy route
  through **type-erased ops** (`TypeErasureOps` in `type.h`:
  `default_ctor`/`dtor`/`copy_ctor`/
  `move_ctor`/`copy_assign`/`move_assign`), which `TypeImpl<T>` (in
  `evaltypes.cpp.h`) implements via
  placement-new + `reinterpret_cast`.
- **`Type`** is a polymorphic operations table, **one singleton per kind**, held
  in the global
  `AllTypes` array (`types.cpp`) indexed by the `Type::TypeE` enum (`type.h`).
  Every operation —
  `add`, `sub`, `mul`, `lt`, `eq`, `is_true`, `to_string`, `len`, `subscript`,
  `slice`, `clone`,
  `hash`, `use_count`, `intptr`, … — is a `virtual` on `TypeTemplate` dispatched
  through the value's
  `type` pointer. The base implementations throw `TypeErrorEx`; a type "gains" a
  behavior purely by
  overriding the relevant virtual (see `src/types/int.cpp.h` for the canonical
  example). Binary ops
  **mutate the left operand in place**
  (`void add(EvalValue &a, const EvalValue &b)` does `a += b`).
- **`bool` is a real scalar type (`t_bool`, `TypeBool` in
  `src/types/bool.cpp.h`).** `true`/`false` are its only two values (parsed to
  `LiteralBool`, `syntax.h`). It is stored in `EvalValue`'s `bval` union member
  (which aliases `ival`'s low byte; the `EvalValue(bool)` ctor zeroes the full
  `ival` first, so reading the slot as the int `0`/`1` is also valid). `bool`
  sits at the bottom of the numeric promotion chain **`bool <= int <= float`**:
  `num_bin_op` promotes a bool operand to `int 0/1` before dispatch, so
  `TypeBool` itself only implements `is_true`/`to_string`/`hash` (the hash
  matches the equal int `0/1`, so `true` and `1` are one dict key). Comparisons
  (`Expr06`/`Expr07`), logical ops (`Expr11`/`Expr12`, via `logop_loc`), and
  unary `!`/`-`/`+` produce a `bool` (`-true`/`+true` promote to int first); the
  `EvalValue` comparison operators read the result via `is_true()`, **not**
  `get<int_type>()` (a comparison result may now be a bool — `TypeArr::noteq`
  was the one internal consumer that had to switch). `MakeConstructFromConstVal`
  and `value_repr` (specialization-dedup key) handle bool. Bool-returning
  predicate builtins (`defined`/`isconst`/`isconstdecl`/`ispure`/`ispuredecl`/
  `startswith`/`endswith`/`isinf`/`isfinite`/`isnormal`/`isnan`) return a real
  bool; `int()`/`float()` accept a bool.
- **Mixed int/float promotion is centralized in `num_bin_op()`**
  (`evalvalue.h`), not in the type
  classes. The type virtuals are single-type: `TypeInt::add` only handles an int
  RHS, `TypeFloat::add`
  also accepts an int RHS (promoting it). `num_bin_op(a, b, &Type::op)` is the
  dispatch chokepoint:
  it first promotes a **bool** operand (either side) to `int 0/1`, then, when
  `a` is int and `b` is float, promotes `a` to float, so `int OP float` lands in
  `TypeFloat` and behaves identically to `float OP int` (and `bool OP x` like
  `int OP x`). **Dispatch binary
  arithmetic/comparison
  through `num_bin_op`, never by calling `a.get_type()->add(...)` directly** —
  every call site does
  (the `ExprNN::do_eval` ladder and compound-assign in `eval.cpp`, `EvalValue`'s
  `== != < <= > >=`
  operators, `builtin_sum`). It is a no-op for any non-`(bool/int,float)`
  operand pair, so string/array/etc.
  comparisons pass through unchanged. (Logical `&&`/`||` and unary ops are *not*
  routed through it.)
  Note for dict keys: an integer-valued float hashes as the equal int
  (`TypeFloat::hash`) so that
  `1` and `1.0`, which compare equal, are the same key.
- **The trivial / non-trivial boundary is `t_str`.** `TypeE` order matters:
  `t_none, t_lval, t_undefid, t_int, t_builtin, t_float, t_bool` (`< t_str`,
  trivial, stored inline,
  bit-copyable) then `t_str, t_func, t_arr, t_ex, t_dict` (`>= t_str`,
  non-trivial, need the
  type-erased lifecycle ops). Hot paths branch on `type->t < Type::t_str` /
  `>= Type::t_str` (e.g.
  `EvalValue::clone()` short-circuits for trivial types). If you add a type, its
  position relative to
  `t_str` decides which machinery applies.
- `t_lval` and `t_undefid` are **internal pseudo-types**, never visible to
  scripts (blank entries in
  `TypeNames`). They tag the two special `EvalValue` payloads below.
- **`LValue`** (`evalvalue.h`) = an assignable slot: an `EvalValue` + `is_const`
  flag + an optional
  back-pointer (`container`, `container_idx`) used when the slot is an *element
  of an array* (needed
  for copy-on-write, below).
- **`RValue(v)`** collapses an `EvalValue` holding an `LValue *` down to the
  contained value, and
  throws `UndefinedVariableEx` if it holds an `UndefinedId`. Builtins and
  operators call `RValue(...)`
  on every operand. `Identifier::do_eval` returns an `EvalValue` wrapping
  `LValue *` when the symbol
  is found (walking the parent chain), else an `UndefinedId{name}` sentinel —
  *not* an immediate
  error, which is what lets `defined()` and declaration-vs-assignment logic
  work.
- **`EvalContext`** (`eval.h`) is a lexical scope:
  `map<const UniqueId *, LValue>` + `parent` pointer
  + `const_ctx`/`func_ctx` flags. The root context auto-loads `const_builtins`
    (always) and `builtins`
  (only when not a const ctx). A `Block` evaluates in a fresh child
  `EvalContext` — **except** a `scope_free` block (the resolver sets the flag
  when every declaration in it is a frame slot: no capture, nested-func name, or
  slot-budget overflow), which never touches the map and so runs its statements
  directly in the parent context, skipping the per-entry `EvalContext`
  build/teardown (a measurable win for loop/if/function bodies, re-entered every
  iteration/call). The root block always builds its context.
- **Slot resolution (`resolver.cpp`) bypasses the map for resolved locals.**
  The post-parse `resolve_names()` pass assigns slot indices so an
  `Identifier::do_eval` for a resolved local is an O(1) read of
  `EvalContext::frame->slots[slot]` instead of the `map`+parent-chain walk. A
  call's `Frame` (an inline slot buffer + heap spill past 8, plus a `uint64_t
  live` bitmask) is created in `do_func_call` when `FuncDeclStmt::resolved`, and
  for the program's implicit "main" by `Block::do_eval` on the root block;
  nested blocks inherit the `frame` pointer.
  **Slotted: a function's params and its locals** (`var`/`const`, `for`-init,
  `foreach`, `catch` variables) **and top-level variables.** **Not slotted (stay
  in the map):** function *names* (so forward references / mutual recursion
  work), builtins, captures, and any **top-level variable a function reads** —
  functions reach globals through the scope-chain map walk, not slots, so the
  resolver's first pass collects those names (`escaped`) and its second pass
  keeps them in the map. Anything unresolved falls back to the map, so the pass
  is purely an optimization. The resolver does a forward
  lexical walk (no hoisting, so `var x = x + 1` reads the outer `x`); each
  `Block` records its slot range and `Block::do_eval` clears those `live` bits
  on entry, so a re-entered loop body's locals start undefined again. Slots
  can't hold the `UndefinedId` sentinel (`LValue` forbids it), hence the `live`
  bitmask for undeclared/`undef()` state, and same-block duplicate declarations
  are caught here (`AlreadyDefinedEx`) so the runtime decl path can just
  overwrite. `Identifier::sym` and `FuncDeclStmt::{resolved, frame_size,
  slot_writes}` carry the results; `slot_writes` (per-slot write counts:
  write-once == 1 for a local, 0 for a never-reassigned param) is what the
  auto-const folder uses to find promotable write-once vars (see the
  const-evaluation section). The const-eval path runs before resolution, so pure
  funcs invoked at parse time use the map (`resolved` is still false then).
  **Slots also bypass the map on the WRITE side:** `handle_single_expr14`
  (`eval.cpp`) fast-paths an assignment / compound-assignment to a resolved,
  live, non-const local — it read-modify-writes `frame->slots[slot]` in place,
  skipping the `lvalue->eval()` → `LValue*` → `doAssign()` round-trip (it falls
  through to that general path when the slot is undefined or const, so the same
  errors still fire). When both the slot and the rhs are ints, a
  compound-assign (`+=`/`-=`/`*=`) does the op **directly** on the slot's int —
  no `num_bin_op` PMF dispatch, no copy in/out (`div`/`mod` stay general, for
  the zero check). And `Expr14::do_eval` has a sibling fast path for `local
  += N` with an **int-literal** rhs: it skips evaluating the literal node too
  (what an `i++` would compile to — there is no `++` operator). The literal is
  recognized by a cheap `is_lit_int()` tag check (`ConstructType::lit_int`),
  **not** a `dynamic_cast` — this path runs on every `i += 1`, so an RTTI
  lookup there is a measurable tax (it dominated tight `while`/`for` loops).
  `foreach` binds a resolved-local loop var the same way via `bind_loop_var`.
  All use `as_resolved_local`, a cheap `is_id()` tag check (`ConstructType::id`),
  not a `dynamic_cast`.
- **`UniqueId`** (`uniqueid.h`) interns identifier strings in a global
  `std::set`; symbols are keyed
  by the interned *pointer*, so lookup is pointer comparison. (Global mutable
  state lives in
  `types.cpp`: `UniqueId::unique_set`, `EvalContext::builtins`, `AllTypes`,
  `empty_str/empty_arr/none`.)

## Evaluation specifics worth knowing before editing `eval.cpp`

- **`return`/`break`/`continue` are signaled via `FlowState`, NOT C++
  exceptions.** Each
  `EvalContext` carries a `FlowState *flow` (`eval.h`) pointing at one
  `FlowState` (a `type` enum + an `EvalValue value`) per *function invocation*:
  function-boundary contexts (`func_ctx`) and the root own theirs, nested
  blocks/loops inherit the parent's pointer (so a fresh one per call —
  recursion never shares).
  `BreakStmt`/`ContinueStmt`/`ReturnStmt::do_eval` just set `ctx->flow->type`
  (and `->value` for ret)
  and return; `Block::do_eval` stops its statement loop the moment
  `flow->type != none`; the loop
  evaluators (`While`/`For`/`Foreach::do_iter`) consume `brk`/`cont` (resetting
  to `none`, with `for`
  still running its `inc` on `cont`) and let `ret` pass through; `do_func_call`
  reads `flow` after the
  body and returns `flow->value`. `finally` (the scope guard in
  `TryCatchStmt::do_eval`) *suspends* an
  in-flight signal around the finally body, then resumes it (unless finally
  raises its own). This
  replaced exception-based control flow because a C++ `throw` costs ~1.6µs here
  (heap alloc + DWARF
  unwinding, irreducible by build flags) and `return` fires constantly — see
  `bench/` for the ~9×
  speedup on recursion. **Only genuinely exceptional control flow still throws
  C++ exceptions:**
  runtime errors (`RuntimeException` subclasses), user `throw`
  (`ExceptionObject`), and `rethrow`
  (`RethrowEx`, defined locally in `eval.cpp`) — caught by
  `do_catch`/`TryCatchStmt`.
- **`Construct::eval()` wraps `do_eval()`** to attach the node's source `Loc` to
  any in-flight
  `Exception` that doesn't already carry one — this is how runtime errors get
  pointed at source.
  Override `do_eval`, not `eval`.
- **Function call scoping is lexical/closure-based.** `do_func_call` binds
  params into an
  `args_ctx` whose parent is the function's **`capture_ctx`**, not the call
  site. A `FuncObject` holds
  the `FuncDeclStmt *` plus that `capture_ctx`, which is parented to the *root*
  context — so functions
  cannot see caller locals or globals, only captures (and, for pure funcs, only
  consts + params).
  Builtins are different: they receive the **caller's `ctx`** and the
  **unevaluated** `ExprList`.
- **Trailing `opt` parameters are skippable at the call site.** The four
  `do_func_bind_params` overloads (`eval.cpp`) accept any arg count in
  `[min_required_args(params), nparams]` and bind each omitted trailing param to
  `none`. `min_required_args` is `1 + the index of the last non-opt param` (0 if
  all opt), keyed off `Identifier::opt_mod` — so a non-opt param *after* an opt
  one raises the minimum and can't be skipped (`f(x, opt y, z)` still needs 3).
  The inferencer's `check_call` enforces the same `[min, nparams]` range
  (`WrongArgCountEx` with a "MIN to MAX" message); no per-call type contribution
  is needed for the omitted params, since an `opt`-declared param is already
  typed `opt T` at finalization (so the body must null-check it). The inliner /
  tail-inliner / specializer all bail on an arg-count ≠ nparams mismatch, so an
  under-arity call simply runs through `do_func_call` at runtime (correct).
- **Const parameters.** A param declared `const` (`func f(const x, y)`, parsed
  by `pFuncParam`, flagged `Identifier::const_param`) is bound as a const
  `LValue`, so reassigning it throws — caught at compile time by the resolver
  (a `const` param with a nonzero body write count → `CannotRebindConstEx`) and,
  as a fallback, at runtime. Params are otherwise bound **mutable** — even
  during const-eval — so a (pure) function may reassign its own by-value params;
  binding const-ness is keyed off `const_param`, *not* `ctx->const_ctx`. A plain
  param the resolver finds is never reassigned (`slot_writes == 0`) is tagged
  `auto_const_param` (effectively const; used by `isconst()`).
- **`clone()` semantics differ by capture.** A non-capturing `FuncObject` clones
  to *itself* (shared
  `shared_ptr`); a capturing one is deep-copied so each clone has independent
  captured state. This is
  the mechanism behind the counter/closure examples in the README.
- **Multiple assignment & array expansion** all funnel through `Expr14` +
  `handle_single_expr14`:
  an `IdList` lvalue with an array rvalue spreads element-wise
  (`var a,b = [1,2]`), with a non-array
  rvalue assigns the same value to each (`var a,b = 0`). The same helper drives
  `foreach`
  tuple-unpacking and the `indexed` keyword.
- **`++` / `--` (`IncDecExpr`, `syntax.h`)** — C-style pre/postfix increment and
  decrement, **int/float only**. `IncDecExpr::do_eval` evaluates the operand
  exactly ONCE via two paths: when the inferencer proved it int/float (`th` is
  `i`/`f` — the usual case, incl. flat-array elements and POD fields that have
  no `LValue`) it routes the mutation through `handle_single_expr14`
  (`operand += 1`), reusing every store fast path (slot, flat array, COW,
  struct), and **derives `old = new ∓ 1`** for postfix so it never re-reads the
  operand; a `dyn`/un-hinted operand (always `LValue`-backed) goes through a
  read-modify-write so the int/float requirement is enforced at runtime. The
  **inferencer** types it (`type_of` = `operand ± 1`) and the **check pass**
  rejects a non-lvalue, a `const`, or a non-int/float operand (bool included) at
  compile time — `var b=true; b++` is a `TypeMismatchEx`, not a silent int.
  The **resolver** counts it as a write (`count_write`), so a `++`'d var is not
  auto-const-promoted; the **inliner** refuses to inline an expression body that
  reassigns a SCALAR param (`func f(x)=>x++` — `mutates_a_param`), since the
  param is a by-value copy (a mutation *through* a param — `p.x++`, `a[i]++` —
  is allowed: that already has reference semantics, so inlining matches the
  call). Lexing is maximal-munch, so `--1` is decrement-of-`1` (a compile error,
  like C), not `-(-1)`.
- **Dict access: throw-on-missing-read, insert-on-write, or default.**
  `TypeDict::subscript(what, key, for_write)` and `MemberExpr::do_eval` (which
  share the logic) handle a missing key by: returning the dict's default (a
  *default dict* from `dict(default_value)`, `DictObject::{has_default,
  default_val}`); else, on a **plain-assignment target** (`for_write`),
  auto-vivifying (insert `none`/default) so `d[k] = v` inserts; else **throws**
  `KeyNotFoundEx` — so a *read* or *compound assign* (`d[k]`, `d.k`, `d[k]+=1`)
  of a missing key in a plain dict throws rather than yielding `none` (the read
  is non-`opt`). `for_write` is `EvalContext::assign_target`, set by
  `handle_single_expr14` only for `op == assign` and **consumed** by the
  outermost subscript/member (so a nested base like `d[k1]` in `d[k1][k2]=v` is
  read, not vivified). `get!()`/`get()` are the explicit fail-fast / nullable
  accessors. A `const` dict folds known-key reads at parse time (a known-missing
  key therefore throws at *compile* time). The clone paths
  (`TypeDict::clone`, `clone_to_mutable`, `make_const_clone`) preserve
  `has_default`/`default_val` (`dict()` stays a const builtin, so `dict(0)`
  folds — hence the clone-preservation matters).

## Copy-on-write containers

Strings, arrays, and dicts are reference-counted with value semantics preserved
via COW. The handle is **`intrusive_ptr<T>`** (`intrusiveptr.h`), not
`std::shared_ptr`: a single-threaded interpreter doesn't need shared_ptr's
two-word layout (object + separate control block) or its *atomic* refcount ops,
so the count lives in the pointee (which inherits `RefCounted`) and retain/
release are plain `++`/`--`. This is what keeps `SharedArrayObj`/`SharedStr` at
24 bytes (so `EvalValue` is 32 and the array element `LValue` is 48) and removes
the atomic-refcount churn from copy-heavy array/dict code. **Gotcha:**
`RefCounted`'s copy/move ctors reset the count to 0 — a cloned object owns a
fresh count, never the original's (else it would never be freed). `use_count()`
keeps shared_ptr's meaning (handles sharing the pointee), so the `> 1` COW tests
are unchanged.

- **`SharedStr`** (`sharedstr.h`): immutable `intrusive_ptr<StrObj{string}>` +
  `off`/`len` slice view (StrObj just wraps the string so it can carry the
  count). Slices are
  cheap views; strings are never mutated in place. Copies are forbidden
  (`= delete`), only moves —
  enforcing the no-accidental-copy intent.
- **`SharedArrayObj`** (`sharedarray.h`):
  `intrusive_ptr<SharedObject{ vec, set<live slices> }>` +
  `off`/`len`/`slice`. A slice registers itself in the parent's `slices` set
  (and unregisters on
  move/destroy). Writing through an array-element `LValue`
  (`LValue::get_value_for_put` in `eval.cpp`)
  triggers COW: if the container is a slice, or is aliased (`use_count > 1` /
  has live slices), it is
  cloned first so the write doesn't bleed across logically-distinct arrays.
  **Length invariant:** for a *non-slice*, `len` is only the size at
  construction and goes stale once `+=`/`append`/`insert`/... grow the vector in
  place — a non-slice reports its length via `size()` (= `vec.size()`), and
  `offset()`/`size()` are the only correct way to read its range. (Bug to avoid:
  `clone_internal_vec` must use `offset()`/`size()`, not the raw `off`/`len`, or
  it truncates a grown array. `clone_aliased_slices` therefore clones each slice
  while its `slice` flag is still set, so `offset()`/`size()` report the slice
  range.)
- **Flat (unboxed) int/float/bool storage.** `SharedObject` carries a `Storage
  kind` (`general`/`ints`/`floats`/`bools`) and an **anonymous union** of `vec`
  (the `vector<LValue>`, 48-byte slots), `ivec` (`vector<int_type>`), `fvec`
  (`vector<float_type>`), and `bvec` (`vector<unsigned char>`, **one byte** per
  element) — the flat members are unboxed, so a homogeneous int/float array
  moves ~6× less memory in bulk ops and a bool array ~48× less. A `union`
  member can't have non-trivial ctors/dtors, so `SharedObject` placement-news
  the live member per kind and the dtor switches on `kind`.
  **Representation is type-driven and fixed at creation — there is NO runtime
  promotion** (`promote_to_general` was deleted). An array-producing node is
  built flat **iff** the inferencer proved its *destination* is
  `array<int>`/`array<float>`/`array<bool>`; a destination that is `array<dyn>`
  (or any other element type) is built **general from the start**. The
  inferencer's
  `set_array_repr_hint` (in `annotate_hints`, runs on `a = <rvalue>` decls and
  assigns) stamps an **`ArrHint`** (`syntax.h`: `dflt`/`general`/`flat_i`/
  `flat_f`/`flat_b`) on the rvalue — on a `range()`/`array()`/`make_array()`
  call's args
  `ExprList`, or directly on an array literal / folded `LiteralObj`. A
  `dyn`-typed destination (`var dyn d = [1,2,3]`) also gets `general`, so
  declaring `dyn` builds a polymorphic array from the start (else a later
  `d[0]="x"` would wrongly hit the flat-array error on an already-`dyn` var).
  Creators honor it: `range`, `builtin_array` (1-arg `flat_i`/`flat_f`/`flat_b`
  → flat `0`/`0.0`/`false` fill, replacing the old `array(N)` rewrite;
  `general` → general),
  `make_array`, `LiteralArray::do_eval`, and `LiteralObj::do_eval` (a flat baked
  literal bound to an `array<dyn>` dest is made general via
  `make_general_array_clone`).
  The fixpoint propagates the destination type through direct aliases
  (`var b = a`), so they agree. Value-driven flat (`dflt`) is the fallback for
  contexts the hint doesn't cover. The flat fast paths (each branches on
  `skind()`, reads `flat_ints()`/`flat_floats()`/`flat_bools()`/`arr_elem_at`
  directly, never
  promotes): `sum`/`reverse`/`sort` (comparator and not — `comparator_heapsort`
  is templated over the element vector)/`min`/`max`/`append`/`pop`/`top`/`find`/
  `erase`/`insert`/`map`/`filter`/`foreach`/`intptr`/`dict`(from pairs)/`join`/
  `writelines`, `TypeArr::{subscript (rvalue read), to_string, eq, add}`,
  `Subscript::eval_int`/`eval_float`, the array-spread reads
  (idlist/`foreach`-tuple, via `arr_elem_boxed`), and the flat subscript-store
  `try_flat_subscript_store` (`eval.cpp`: `a[i] = v` / `a[i] OP= v` writes the
  scalar straight into the flat vector — gated on a side-effect-free lvalue
  *chain* via `no_side_effects`). `arr_elem_at`/`arr_elem_boxed` box a flat bool
  element as a real `bool`; `sum` of an `array<bool>` returns an int (counts the
  trues). `clone_internal_vec`, `make_const_clone`, and
  `clone_to_mutable` are kind-aware so clone/COW/const keep flat. `size()` is
  kind-aware. **`get_vec()`/`get_view()` are general-only** — they throw
  `InternalErrorEx` on a flat array (an invariant tripwire, not a promotion);
  every caller either guarantees general or reads flat directly. **The
  dyn-launder error:** the only way a flat (statically-typed) array can be asked
  to hold a non-fitting element is by laundering it through a `dyn` alias and
  mutating it (`var dyn d = int_array; append(d, "x")` / `d[0]="x"` / `insert`).
  Since the storage stays int-typed and an alias-affecting write can't change
  its representation without promotion, that **throws a `TypeErrorEx`** (message
  `flat_array_violation_msg`) — declare the array `dyn` from the start, or
  promote an existing one with the **`dynarray(a)`** builtin
  (`builtin_dynarray`: a fresh general copy, typed `array<dyn>`, usable in any
  position; `clone`/`deepclone` deliberately preserve the layout). `runtime()`
  does *not* promote — it only relabels the static type, not the storage.
  `array_storage(a)` reports
  `"ints"`/`"floats"`/`"bools"`/`"general"` (tests pin it). **Gotcha:** any pass
  that
  inspects a const array's element type must read from `skind()`, not
  `get_view()`/`get_vec()` (now they'd throw on flat anyway). `array()` is a
  **non-const** builtin (never folds to a baked literal, so `array(N)` is always
  a runtime call the hint reaches, and a huge `array(1000000)` isn't baked). See
  `plans/typed-arrays.md` (approach B) and
  `plans/type-driven-specialization.md`.
- **`DictObject`** (`shareddict.h`): the value handle is
  `intrusive_ptr<DictObject>` (the object inherits `RefCounted`); the map lives
  inside it.
- **Deep-const read-only flag.** Both `SharedObject` (arrays) and `DictObject`
  carry a `readonly` bool (`is_readonly()`/`set_readonly()`). It backs `const`
  values: `make_const_clone()` (`eval.cpp`) sets it on every array/dict in the
  value, recursively, so `const` is *deep* read-only. The flag lives on the
  shared object, so it travels with every alias and slice; `clone()` builds a
  fresh object and is therefore always mutable (`TypeDict::clone` clears it
  explicitly; `TypeArr::clone` gets a new `SharedObject` for free). Write paths
  check it: `TypeArr::subscript`, `TypeDict::subscript` and `MemberExpr` return
  an *rvalue* (and don't auto-vivify) for a read-only container, so the
  element/member
  assignment fails with `NotLValueEx`; `TypeArr::add` (`+=`) and the mutating
  builtins (`append`/`pop`/`insert`/`erase`) throw `CannotChangeConstEx`;
  `sort()` clones instead of sorting in place. This is what makes a const
  immutable through a non-const alias (e.g. a function parameter) — parse-time
  read-folding alone didn't. (Aside: `builtin_sum`'s general path must seed its
  accumulator with a `clone()` of the first element, since `+=` mutates it in
  place — it would otherwise mutate, or be rejected on, a read-only argument.
  `builtin_sum` also has an all-int fast path that accumulates a raw `int_type`
  in a tight loop — skipping `num_bin_op`'s promotion check and the per-element
  virtual `TypeInt::add` — and falls back to the general loop at the first
  non-int element, so a mixed int/float array still promotes correctly.)
- **Getting a mutable copy of a const: `clone()` vs `deepclone()`.** Two helpers
  in `eval.cpp` make mutable copies (scalars/strings returned as-is):
  `make_mutable_clone` builds a fresh mutable *top* but **shares** any read-only
  sub-object as-is, while `make_deep_mutable_clone` copies every level and drops
  `readonly` (a fully independent writable value). `make_mutable_clone` backs
  the per-eval copy a `var`-bound materialized value needs, and its
  share-the-const behavior is what keeps **`clone()` shallow** (a const nested
  in the result stays read-only) and makes const-ness propagate into fresh
  literals (`var a = [y]` with `y` const keeps `a[0]` read-only). The `clone()`
  builtin is the type's own shallow `clone` (one level); `deepclone()` (a
  runtime builtin, `make_deep_mutable_clone`) is the deep one — the way to
  obtain a fully mutable version of a const. (`deepclone` is *not* a const
  builtin: it yields a mutable value that must be copied fresh per eval anyway,
  so folding it would only bloat the tree.)
- The non-const `intptr(symbol)` builtin exposes the underlying object pointer;
  the test suite uses it
  to assert exactly when two slices do/don't share storage. If you change COW
  logic, those tests are
  your spec.

## Custom struct types

User-defined value types (`struct Point { int x; int y; }`). Full design +
phasing: `plans/structs.md`. **Status: complete (all 8 phases)** — decl,
construction, field/const access, inference, const-folding, COW; the POD
C-layout; nested (recursive) POD; flat `array<PodStruct>` storage; M8-style
*direct* (unboxed) field access; and construct-in-place append. A flat struct
array is now both a memory/cache win and array<int>-speed on field ACCESS
(`a[i].x` reads straight from the bytes, within ~6% of `array<int>`'s `a[i]`).
Construction stays more expensive than an inline int (a struct is an object),
but the per-element `StructObject` allocation is gone (build overhead
2.47x→1.76x vs `array<int>`; `bench/58_structs` ~26x→~21x CPython).

- **POD vs boxed storage** (`StructTypeDef::compute_layout`, run by the parser).
  A struct is **POD** iff every field is a non-opt scalar (`bool`/`int`/`float`)
  **or a non-opt POD struct** (embedded *inline*, recursively); it then has a
  native C byte layout (`pod_field_metrics` assigns offsets with our own fixed
  alignment rules — `pod_field_size`/`align` in `structtype.h`, the one place an
  arch assumption lives, depending only on `sizeof(int_type)`). Any ref/opt/
  boxed-struct field makes it **boxed** (a `vector<LValue>` slot array). A
  nested struct field embeds inline only if its type was *declared before* this
  one (resolved via `const_ctx` in the parser → `FieldDef::struct_def`); a
  forward/self reference to a **non-recursive** type stays a boxed pointer.
- **Recursive-struct rejection** (`check_struct_no_recursion`, parser, run
  before `compute_layout`). A **non-opt** struct field whose type — directly or
  transitively through other non-opt struct fields — contains the struct itself
  is a compile error (`SyntaxErrorEx`, "box it as `dyn? <name>`"): such a value
  can never be constructed (infinite nesting; and inlined, infinite size). The
  field-reference graph among earlier structs is a DAG (inline only goes
  backward), so the only back-edge that can close a cycle is a forward/self
  reference to the struct being declared — detected by a DFS over non-opt struct
  fields (`struct_field_target` resolves each edge by `struct_def`, the
  root's own name, or a `const_ctx` lookup for an intermediate forward ref). An
  **`opt`** field (e.g. `dyn? next`) breaks the cycle (it can terminate with
  `none`) and is allowed — the way to write a linked list / tree.
- **`StructObject`** holds EITHER `bytes` (POD: a `def->size` C-laid-out buffer;
  `pod_get`/`pod_set` load/store a typed scalar or an inline nested struct at a
  field offset) OR `fields` (boxed). A POD field WRITE goes through
  `try_pod_struct_store` (a direct byte store, mirroring
  `try_flat_subscript_store`); a POD field READ in `MemberExpr` returns a value
  (no per-field LValue). `==` is `memcmp` for POD, field-wise for boxed.
- **Flat `array<PodStruct>`** — `SharedArrayObj::Storage::structs`: a contiguous
  byte buffer + the element `StructTypeDef*` + cached `stride` (so the template
  never needs `StructTypeDef` complete). Created value-driven (a literal of
  same-type POD structs → `LiteralArray` mode 5) or type-driven for an empty
  `[]` whose destination is `array<PodStruct>` (`ArrHint::flat_s` +
  `Construct::arr_hint_struct`, honored by `LiteralArray`/`LiteralObj`), so
  `var a=[]; append(a, S(..))` stays unboxed. Hot paths touch bytes directly
  (subscript read/store, append, `==`, `to_string`, clone/const-clone,
  `sty_from_value`); **`foreach` reuses one `StructObject`** across iterations
  (overwrite-in-place with a `use_count` COW guard, so a captured element keeps
  its value). Cold ops (insert/sort/map/...) **auto-promote** to a general array
  via `get_vec()`'s `promote_structs_to_general()`, so every existing array op
  works with no dedicated case and nothing throws. `array_storage` reports
  `"structs"`.

- **Direct (unboxed) field access** (M8, phase 8). The inferencer stamps a
  `TypeHint` on `s.x` when the field is a non-null int/float; `MemberExpr::
  eval_int`/`eval_float` then read it unboxed. `a[i].field` on a flat struct
  array reads the scalar STRAIGHT from the array bytes
  (`member_pod_array_scalar`) with **no per-element `StructObject`** built
  (guarded by `no_side_effects` so the base evals once) — `a[i].x` is within ~6%
  of `array<int>`'s `a[i]`. The resolved-local compound-assign fast path
  (`sx += rhs`) treats a `th==i` rhs (e.g. `p.x`) as an `eval_int()` read, so a
  reduction is fully unboxed.

- **Construct-in-place append** (phase 8). `append(flat_struct_arr, S(...))`
  recognizes a struct-constructor arg for the array's exact POD type and builds
  the element straight into the array's byte buffer
  (`try_construct_into_struct_array`, `eval.cpp`) — no temporary `StructObject`.
  Field args are coerced into a stack buffer first (a throw mid-construct leaves
  the array unchanged), then committed. Named/mixed args (already desugared) and
  nested POD work; a non-constructor / different-type arg falls back to the
  normal value-append (the arg is never evaluated twice). The byte store is the
  shared `pod_store_field` (also backs `StructObject::pod_set`).

- **Two value kinds, mirroring func's decl/object split** (`type.h` `TypeE`):
  `t_structtype` (trivial, `< t_str`, a raw `StructTypeDef *`) is the **type
  descriptor** the `struct` decl binds as a `const` in scope — callable
  (construction) and `.CONST`-accessible; `t_struct` (non-trivial, `>= t_str`,
  `intrusive_ptr<StructObject>`) is an **instance**. `TypeStruct` /
  `TypeStructType` in `src/types/struct.cpp.h`; wired through `ValueU`,
  `TypeToEnum`, `TypeNames`, `AllTypes`.
- **`structtype.h`** defines `StructTypeDef` (AST-owned, program-lifetime:
  `name`, `fields` as `FieldDef{name, FieldKind, struct_ty, is_opt, slot,
  offset}`, folded `consts`, plus the `Layout`/`size`/`align` fields the POD
  phases will fill) and `StructObject` (`RefCounted`: `def`, `readonly`, and a
  boxed `vector<LValue> fields`). `evalvalue.h` forward-declares both.
- **Parser** (`pAcceptStructDecl`, `parser.cpp`): the `struct` keyword
  (`kw_struct`) → a `StructDeclStmt` (owns the `StructTypeDef`;
  `do_eval` binds the descriptor like a func name, so it works inside a function
  too). Fields are `[opt] TYPE name;` (a primitive keyword, `dyn`, or a
  struct-type name → `FieldKind`); `const NAME = expr;` members fold immediately
  (`make_const_clone`). v1 rejects `var` fields and `opt` on a non-ref field;
  and duplicate member names.
- **Construction is a call**: `Point(...)` parses as a `CallExpr`; what makes it
  construction is only that the callee is a struct descriptor (decided in the
  inferencer + at eval, never in the grammar). It reuses the named-arg pipeline:
  the inferencer's `lower_named_args` and `check_call` recognize a struct callee
  and build the `ParamSpec`/param-type list from the fields, so positional /
  named / mixed and the arity-range / per-field-assignability / non-opt-not-none
  checks all come from the shared machinery. `CallExpr::do_eval` →
  `construct_struct` builds the boxed instance; `coerce_struct_field`
  coerces a numeric field and **runtime-validates** each field's type (guarding
  a `dyn`-laundered value; the `dynarray`-style escape hatch is just declaring
  the field `dyn`). `type_of(Point(..))` is `STyKind::Struct` (`stype.h`'s
  reserved `Struct` kind + `struct_def`/`struct_name`; `STyArena::struct_ty`).
- **Member access** (`MemberExpr::do_eval`): dispatch on the base — a `t_struct`
  instance → `def->slot_of(memUid)` field (an lvalue when mutable, an rvalue for
  a read-only/const instance so a write fails `NotLValueEx`) else a `const`
  member else not-found; a `t_structtype` descriptor → only `const_of`; a dict
  unchanged. `MemberExpr::memUid` (interned `UniqueId`, set in `pAcceptMember`
  beside the dict-key `memId`) drives the slot lookup. The inferencer types
  `s.field`/`Type.CONST` and validates membership; a `dyn`-base read resolves at
  runtime via the tag.
- **`const` works fully**: a struct construction folds **inside a `const` decl**
  (`construct_struct`'s own validation makes it safe), baked deep read-only by
  `make_const_clone`; `MakeConstructFromConstVal` / `clone_to_mutable` /
  `is_readonly_value` / `ShouldConstSymbolExistAtRuntime` / `sty_from_value` all
  handle a struct value. Outside a `const` decl, construction is left a runtime
  `CallExpr` so the inferencer gives the precise field errors.
- **Value semantics**: COW like arrays/dicts (`StructObject::readonly` backs a
  deep `const`; plain assignment aliases; `clone()` shallow, `deepclone()`
  deep). `==`
  is structural between same-`def` instances (`TypeStruct::eq`); `hash` throws
  (not hashable, v1). **Deferred** (plans/structs.md): `var` fields (call-site
  field inference), `opt` scalar fields, methods, and empty structs.

## Error model

`errors.h` defines an `Exception` base (`name`, `msg`, `loc_start`, `loc_end`)
and two macros:

- `DECL_SIMPLE_EX` — parse-time / internal errors (`SyntaxErrorEx`,
  `InternalErrorEx`,
  `CannotRebindConstEx`, `ExpressionIsNotConstEx`, …). **Not catchable** from
  script.
- **Compile-time type errors** (`TypeMismatchEx`, `NullabilityEx`,
  `WrongArgCountEx`, `DynRequiredEx`, `OptRequiredEx`) — from the inferencer
  (see
  "Static type inference"). Plain `Exception`s (not `RuntimeException`s), so
  **not catchable** from script; each carries a custom interned message + `Loc`.
  A statically provable type error is reported here, before the program runs.
- `DECL_RUNTIME_EX` — subclasses of `RuntimeException` (adds `clone()` +
  `[[noreturn]] rethrow()`):
  `DivisionByZeroEx`, `TypeErrorEx`, `OutOfBoundsEx`, `KeyNotFoundEx`,
  `NotLValueEx`, `NotCallableEx`,
  `AssertionFailureEx`, `CannotOpenFileEx`, `InvalidValueEx`. These are the ones
  script `try/catch`
  can handle (matched by name).
- **User exceptions** (`ex("Name", payload)`) are `ExceptionObjectTempl`
  (`exceptionobj.h`), a
  `RuntimeException` whose C++ type name is `"DynamicExceptionEx"` but whose
  *script-visible* name is
  the dynamic `dyn_name`. `try/catch` in `eval.cpp` (`do_catch`) clones the
  in-flight exception, then
  matches catch clauses against that dynamic name; `finally` runs via a scope
  guard; `rethrow`
  re-throws the saved exception with the rethrow site's `Loc`.
- Always pass `Loc start, end` to thrown exceptions where you can, so
  `mylang.cpp` can render the caret.

### Error location & caret rendering

- **A `Loc`'s `end` is "last-char-column + 2"**, so the caret width is
  `loc_end.col - loc_start.col - 1` (and the printed end column is
  `loc_end.col - 1`). A construct that ends with a closing token sets
  `end = <that token's loc> + 2` (e.g. `CallExpr`/`Subscript`/`Slice` through
  their `)`/`]`, array/dict literals through `]`/`}`). Keep this convention when
  adding constructs, or carets will be off by one or two.
- **`Construct::eval`** stamps a node's `start`/`end` onto any escaping
  exception that has no loc yet — so an error gets the loc of the *innermost*
  node whose `eval` it traversed. Because `RValue()` and the type ops throw with
  *no* loc, that used to be the whole enclosing expression. The operator ladder
  (`eval.cpp`) now routes operand evaluation through `num_binop_loc` /
  `logop_loc` / `stamp_operand_loc`, and `Expr14`/`CallExpr` wrap their
  rhs/callee evals, so undefined-variable / type / division errors point at the
  **offending operand** (e.g. `var y = foobar` marks `foobar`, not `y =`).
- **Multi-line spans**: `dumpLocInError` (`mylang.cpp`) renders every source
  line in `[loc_start.line, loc_end.line]` with a caret row per line (start from
  `loc_start.col` to EOL, full middle lines, end line up to `loc_end`).
- **Context keywords**: `break`/`continue`/`return`/`rethrow` outside their
  valid context (gated by `pFlags` in `pStmt`) raise a clear `SyntaxErrorEx`
  ("... only allowed in a loop", etc.), not a generic "unexpected token".
- **Not-callable vs undefined**: a var used as a callee is excluded from
  auto-const promotion (`prescan_blocked` blocks `CallExpr::what`), so calling a
  defined non-function reports `NotCallableEx`, not a bogus "undefined var".
- **Uncaught user exceptions** print their throw-site loc + caret and their
  payload (`mylang.cpp`'s `ExceptionObject` handler uses `get_data()`).
- **Backtrace.** `Exception::backtrace` (a `vector<BacktraceFrame>`, `errors.h`)
  is filled as the exception unwinds: `do_func_call`'s `catch (Exception &)`
  records each frame innermost-first, capturing the function's name+params **as
  strings** (the AST is torn down during unwinding, before the top-level handler
  runs; the name is `FuncDeclStmt::display_name` when set — a specialization
  clone's original name — else `id`) plus the *call site* (the `CallExpr`'s loc,
  passed as `do_func_call`'s
  `call_site`). `format_backtrace` (`backtrace.cpp` / `backtrace.h`) renders it:
  frame `[0]` is the innermost (its line = the error site, `loc_start`), each
  deeper frame's line is where it called the next, and a synthetic `main()` is
  the bottom. Two passes: pass 1 builds each `name(params)` (param list
  truncated to ~60 cols as `name(p1, ..., ...)`, the name never cut) and finds
  the widest; pass 2 zero-pads frame numbers to a common width (only when >9
  frames) and right-pads the name column so `at line N` aligns. It is a plain
  function so tests can format synthetic/real backtraces and assert on them.
- **Inlined (virtual) frames.** For function inlining, a node spliced from an
  inlined body carries an `InlineCtx` "inlined-at" chain
  (`Construct::inline_ctx`); `flush_inline_frames` (`backtrace.cpp`) appends one
  `BacktraceFrame` per chain element so the physically-absent inlined calls
  appear. It is flushed at two error-path points, both keyed off
  `Exception::inline_origin_emitted` (not the loc once-guard, which many errors
  pre-satisfy): `Construct::eval` at the innermost node (an error *inside*
  inlined code) emits the chain once and sets the flag, and `do_func_call`'s
  catch (a real call made *from* inlined code) flushes the call-site chain
  unconditionally and sets the flag so the enclosing `CallExpr` doesn't re-emit.
  `format_backtrace` is untouched. See `plans/function-inlining.md`.
- **Tests** pin caret spans via the `test` struct's
  `ex_col`/`ex_line`/`ex_col_end`/`ex_line_end` (each checked only when nonzero;
  see the "err loc:" tests in `tests.cpp`); the "backtrace:" `extra_checks`
  cover the formatter (including synthetic inlined-frame reconstruction).

## The interactive REPL

`mylang` with no FILE/`-e` on a TTY (or `--repl` to force it off a TTY, for
testing) runs the REPL (`run_repl`, launched from `mylang.cpp`). Full design +
status: `plans/repl.md`. Four TUs, split so the logic is headless-testable
behind a thin terminal shell:

- **`repl.{h,cpp}` — `ReplEngine`**, the headless evaluation core. Holds the
  persistent interpreter state: a persistent **const-eval `EvalContext`** + a
  persistent **runtime global `EvalContext`** (both roots, so they auto-load
  the builtins) + the **retained input ASTs** (`vector<unique_ptr<Construct>>`
  — never freed, so a prior pure func / struct / kept const stays valid and
  foldable for later inputs) + the ever-growing source `lines` (for error
  carets). `eval_input(src)` parses against the persistent const ctx with
  `pBlock(pc, 0, /*push_const_scope=*/false)` (so top-level consts/pure-funcs/
  structs survive — the parser drops a const *scalar* decl, but its *value*
  lives in the const ctx, which is what folding reads), then evaluates each
  top-level statement **directly in the persistent runtime ctx** (no fresh
  Block context/frame — that's why state persists and a redeclaration can
  rebind), capturing `print` output (via a `cout` rdbuf swap) and echoing the
  last non-`none` value as `=> ...`. Errors are caught per input and the loop
  continues. The whole-program optimizers (`resolve_names`/inliner/`infer_types`
  /`specialize_types`) are **not** run per input yet (see "deferred" below), so
  top-level names are map-based globals.
  - **Inputs auto-terminate** (`repl_auto_terminate`): you don't type `;` at a
    prompt. Per physical line a `;` is appended unless the line is empty/
    comment-only, ends inside an unclosed `(`/`[`, or ends on a continuation
    token — and a `{` is classified as a statement **block** vs a dict
    **literal** by whether the previous token expected a value, so a multi-line
    func/if body gets interior `;` but a multi-line dict does not.
  - **`EvalContext::allow_redeclare`** (set only on the REPL global scope) makes
    a re-declaration rebind the global instead of throwing `AlreadyDefinedEx`
    (the script rule is unchanged — the resolver still catches same-scope dups
    at compile time). **`pBlock`'s `push_const_scope`** flag (default true) is
    the const-ctx analogue. **Gotcha:** the lexer stores `string_view`s into the
    source lines, so all lines of a multi-line input are appended to `lines`
    *first*, then lexed from the stable buffers (growing-while-lexing dangles
    earlier tokens — an ASan-caught UAF).
  - **Meta-commands** (`eval_input` dispatches a leading `:`): `:tree <code>`
    (parse + serialize, non-committing — parsed with `push_const_scope=true` so
    prior consts fold but new decls land in the popped scope), `:analyze
    <code>` (the colored optimization view), `:source <file>` (split into
    complete units via `is_incomplete` and replay each through
    `do_eval(echo=false)`), `:help [topic]` (the documentation system,
    `replhelp.cpp`), `:trace [<cat>...] on|off` (toggle the diagnostic
    tracer), `:globals` (a table of every global — vars/consts/funcs/structs —
    with its inferred/declared type, merging the runtime scope with the const
    context so folded const SCALARS still appear), `:type <expr>` (a committed
    global's inferred static type via `ReplInfer::global_type`, else the
    runtime structural type of the expression evaluated in a throwaway child
    scope), `:show <function>` (the optimized-AST decompiler, `coderender.cpp`
    — renders the function and its `<name>$N` clones as synthetic code),
    `:quit`.
  - **`completions(buf, cursor)`** — Tab candidates: keywords + builtins + REPL
    globals (`EvalContext::collect_symbols`), or a struct value/type's fields/
    consts right after `base.`.
- **`lineedit.{h,cpp}` — the hand-rolled, IRB-style multi-line editor.** A
  **pure `LineEditor`** core driven one byte at a time via `feed()`: the edit
  buffer **holds embedded newlines** (a multi-line block) and the cursor is an
  offset with a 2-D view (`cursor_row`/`cursor_col`). **Enter SUBMITS only when
  a `Submitter` callback says the buffer parses complete**, else inserts a
  newline + auto-indents by bracket depth (`newline()`); **UP/DOWN move within
  the block** (same column, prev/next line via `move_up`/`move_down`) and fall
  through to history only at the first/last line; **Home/End and the kill keys
  are line-relative** (`line_start`/`line_end` — including the `ESC[1~`/`ESC[4~`
  variants). With no `Submitter` set it is the old single-line behavior, so the
  feed-based unit tests are unchanged. It emits no output and touches no fd, so
  it is unit-tested with raw byte scripts (assert `buffer()`/`cursor_row()`/
  `cursor_col()`). `read_line` is the only non-pure part: termios raw mode via
  an RAII guard, byte-at-a-time read, and the **2-D repaint** (each logical line
  under its `>>`/`..` prompt, the cursor positioned in two dimensions, the prior
  block cleared) — it assumes each logical line fits the terminal width (no
  soft-wrap). Off a TTY it accumulates physical lines until complete. History
  stores whole logical inputs, so UP recalls an entire block.
- **`highlight.{h,cpp}`** — `highlight_line`, a self-contained scanner (NOT the
  lexer; tolerates mid-edit input) that wraps keywords/strings/numbers/comments/
  type-words in ANSI color, preserving the bytes exactly otherwise.
- **`errfmt.{h,cpp}`** — `format_exception`/`dump_loc_in_error`, the
  per-exception-type caret/backtrace rendering, factored out of `mylang.cpp`
  (parameterized by an `ostream` + the source `lines`) so the file driver and
  the REPL share it.
- **`replhelp.{h,cpp}`** — `repl_help(topic, color)`, the `:help`
  documentation system: a self-contained STATIC database (no interpreter
  state) of every builtin (`{name, category, signature, summary, long?}`) and
  of the language features — surface (values, vars, control, functions,
  arrays, dicts, strings, structs, exceptions, the type system) *and* the
  optimization passes (const-fold, auto-const/pure, inlining, specialization,
  template instantiation, M8, flat arrays, CSE, COW), each with a syntax
  sketch + prose and a pointer to its `:trace` category. `:help` overview,
  `:help builtins[/<cat>]`, `:help <builtin>`, `:help language`,
  `:help <category>`, `:help <feature>` all route through `repl_help`, plus a
  **commands DB** (`:help commands`, `:help <command>`) documenting the REPL
  meta-commands themselves. A leading `:` is stripped and means "this is a
  command" (`:help :trace`); a bare topic resolves builtin → command →
  language-category → feature, and when a builtin and a command share a name
  (`trace`/`type`/`globals`) the builtin entry shows with a pointer to the
  command. Feature ids are kept distinct from category ids. The trace category
  list (names + descriptions) has one source of truth, `trace_categories_help()`
  (`trace.cpp`), rendered as an aligned bullet list by `:trace help` and the
  `trace`/`:trace` entries alike. Pure/headless, so it is unit-tested (`replhelp:`
  extra_checks). See `plans/repl-introspection.md`.
- **`coderender.{h,cpp}`** — `render_func_code(fn)` /
  `render_construct_code(c)`, the optimized-AST **"decompiler"**: it unparses
  the FINAL tree (after parse/fold/inference/`resolve_names`/`specialize_types`)
  back into synthetic MyLang-like code, so you see what actually runs — dead
  code gone, folded consts as literals, inlined call bodies spliced in
  (annotated `inlined f`), flat-array element types as `array<int>`,
  typed-scalar/annotation hints as the var's type. A precedence-aware
  expression printer + statement walker, with a comment fallback for any
  unhandled node (best-effort, NOT round-trippable). Backs the `show(f)` builtin
  and the REPL `:show <name>` (which also renders the `<name>$N` clones). It
  reads literal values via the public `ival()`/`fval()`/`bval()`/`strval()`/
  `literal_value()` accessors on the `Literal*` nodes. `render_func_code` takes
  an optional per-param inferred-type list: `:show` passes
  `ReplInfer::func_param_types` (the instance's `FuncInfo::params` types,
  empty for an un-instantiated template), so a template instance renders
  `int func dot$0(int x, int y)` (param **and** return types, via
  `func_param_types`/`func_return_type`) while the base shows untyped
  `func dot(x, y)`; the `show()` builtin (no persistent inferencer) renders
  AST-hint types only. `show()` / `:show` also accept an **expression** (not a
  function): `render_construct_code` renders its optimized tree
  (`:show 2 + 3 * 4` → `14`; `:show <expr>` parses + `resolve_names`'s the arg
  non-committing). `:show` output is **syntax-highlighted** via `highlight_line`
  (extended to color C-style block comments and treat `$` as an id char — the
  `f$0` names). See `plans/repl-introspection.md`.
- **`trace.{h,cpp}`** — the **diagnostic tracer** ("MyLang's mind"): a per-
  category bitmask (`TraceCat`: infer/inline/specialize/template/autoconst/
  autopure/arrays/fold) in `g_trace_mask`, the hot guard `trace_enabled(c)`,
  and `trace_emit(c, indent, msg)` to a swappable sink (default `&std::cerr`).
  The `TRACE(cat, indent, msg)` macro builds `msg` ONLY when the category is on
  — so the guarded emits sprinkled at optimizer decision points (inferencer
  `commit_round`/finalize, and — Pillar 3b — the resolver's inliner /
  auto-const / auto-pure / specializer, the array-hint and fold sites) cost
  one mask test when off. Control surface (both, builtins-first): the
  `trace()`/`traceoff()`/`tracing()` builtins (`builtins/reflect.cpp.h`) and
  the REPL `:trace [<cat>...] on|off` meta-command. The REPL points the sink at
  its per-input capture stream (a `TraceSinkGuard` in `do_eval`) so an enabled
  trace narrates into the REPL output just above the result (and is testable);
  a script leaves it at `cerr` so trace never corrupts stdout. OFF by default,
  so scripts/tests are unaffected. See `plans/repl-introspection.md`.

`run_repl` (in `repl.cpp`) drives it: history loaded/saved to
`~/.mylang_history`, colors gated on a TTY + `NO_COLOR`, Ctrl-C drops the
current input, Ctrl-D at the prompt exits. The editor owns multi-line
continuation (its `Submitter` wraps `ReplEngine::is_incomplete`, treating a
leading-`:` meta-command as always complete), so `run_repl` reads one whole
logical input per `read_line` — no per-line accumulation loop. **A function or
struct can be REDEFINED at the prompt** (the edit-and-resubmit workflow):
`EvalContext::allow_redeclare` is set on *both* the runtime and const scopes
(structs/pure funcs register in the const ctx at parse time), and the
`FuncDeclStmt`/`StructDeclStmt` eval paths erase-then-rebind under it instead of
throwing `AlreadyDefinedEx`. A plain `var`'s TYPE still sticks (the inferencer's
job — see the type-commitment above; `undef` resets it). **Redefining a function
GCs its now-orphaned template/spec instances** (`gc_redefined_instances` in
`do_eval`): an instance (`f$0`) created only by a throwaway top-level call
(`f(1,2)` at the prompt) is removed from both scopes + the inferencer when its
base `f` is redefined, so `globals()`/`specializations(f)`/`:show f` stop
showing it; an instance still **consumed by a function body** is kept (else
calling that function would break). "Consumed" is tracked soundly in the
inferencer: `collect_calls` carries an `in_func` flag (a call below a
`FuncDeclStmt` in the complete `for_each_child` walk), and `instantiate_round`
sets `FuncInfo::has_func_consumer` when it redirects an in-function call to the
instance; `ReplInfer::instance_has_consumer` exposes it. Only instances present
*before* the input (so not the new ones it just created) and whose base name
this input redefined are candidates.

**Faithful per-input pipeline.** `do_eval` runs the REAL pipeline on each input
(after parse): `ReplInfer::check_input` (type inference + checking) →
`resolve_names(..., repl_mode=true)` → `specialize_types` → eval. So the REPL is
the true interpreter — flat arrays, M8 specialization, inlining, slotted nested
locals all happen and are inspectable (`:analyze`).
- **`ReplInfer`** (`inferencer.{h,cpp}`) is a persistent `Inferencer`: the
  one-shot `run()` is factored into `setup()` (once) + `infer_one(root)`
  (per root), and `infer_input(root)` runs `infer_one` then marks this input's
  new `TypeSym`s/`FuncInfo`s **`pinned`**. A `pinned` symbol is a committed
  global: the fixpoint (`reset_round`/`commit_round`/finalize/the `enforce_*`
  passes) **skips** it, and `contribute()` instead **checks** an assignment is
  assignable to its committed type — the cross-input **type commitment** (`var
  x = 3` then a later `x = "hello"` is a `TypeMismatchEx`, "has type" for an
  inferred commit vs "is declared" for an annotated one). All `pinned` branches
  are no-ops in the one-shot path, so scripts/tests are byte-identical. A
  rejected input rolls back (`infer_input` restores the global scope + pins the
  half-built syms). `undef(x)` → `ReplInfer::undef_global` drops the name so a
  later `var x` of a new type is fresh (the REPL diffs the global symbol names
  before/after eval to find what `undef` removed). REPL redeclaration is **not**
  a feature: a re-declared global hits the type-commitment check, and `undef`
  is the way to change a global's type.
- **`resolve_names`'s `repl_mode`** keeps EVERY top-level decl in the map as a
  persistent global (never slotted into "main", never auto-const-promoted — the
  open-world soundness point); nested function locals slot/inline/specialize
  normally. `eval` runs the input's elems directly in the persistent global
  scope, so globals persist and nested calls build their own frames.
- **`render_analysis`/`anno_code`** moved from `mylang.cpp` to `analyzer.cpp`
  (an `ostream` param) so `-a` and `:analyze` share one renderer.

**Tests:** all headless. The **`repl:`** tests drive ONE `ReplEngine` through a
sequence of `(input, expected-substring)` steps, so the persisted global scope
AND the cross-input type commitment / `undef` reset / per-input optimizers are
exercised; the **`lineedit:`** / **`highlight:`** `extra_checks` feed byte
scripts / strings to the pure cores. Only `read_line`'s few syscalls are not
unit-tested. **Not yet (Phase 5):** an IRB-style dropdown completion menu,
reverse-search / bracketed paste, and the Windows raw-input backend.

## Optimizations must generalize (the bar is a compiler, not an example)

The compile-time optimizations (const-fold, auto-const, auto-pure, inline,
specialize, DCE, short-circuit) exist to do for MyLang what a `-O3` compiler
does: **everything knowable at compile time should fold.** A recurring failure
mode here was an optimization that passed its one hand-written test but did
**not** generalize — it worked "pro forma." Several shipped silently broken
(auto-pure that didn't propagate through a call so a 2-deep pure chain didn't
fold; const-fold / inline / specialize that only saw the *current* REPL input so
a call to a prior-input function never folded; a const-false `if` with no `else`
that left a NULL statement and broke the *next* line; no short-circuit folding at
all, so a `const FLAG=false; if (FLAG && …)` guard was never eliminated). Each
was found by a user in minutes of REPL play, not by the suite. So, when you add
or touch ANY optimization, a passing example is the START of the test set, not
the proof. Before calling it done:

- **Test CROSS-INPUT, not just single-compilation.** `check()` (the whole `-rt`
  suite) joins all of a test's source lines into **one** compilation, so it
  *structurally cannot* catch a pass that only sees the current input — yet the
  REPL compiles each input separately and is where users actually hit this. An
  optimizer that doesn't bridge inputs (the `prior_pure` / `prior_scope`
  plumbing on `resolve_names`/`AutoConst`/`Inliner`) regresses there invisibly.
  Add a **`repl:`** test that defines a helper in one input and exercises the
  optimization from a LATER input (`:show` the result). This is the single
  highest-value rule: every cross-input gap above was invisible to single-
  compilation tests by construction.
- **Test COMPOSITION and TRANSITIVITY, not one shape.** A rewrite that removes
  or replaces a node must be tested *followed by another statement*, as the last
  statement, inside a function body, nested, and **chained** (`g`→`f`→`h`,
  partial-const, a side-effecting operand). The bugs live at the seams: a
  folded-away statement that returns NULL breaks the next one; auto-pure that
  doesn't cross a call breaks the chain; an expression-bodied function body that
  a fold pass skips never folds. One example proves nothing about the seam.
- **Use C++ `-O3` as an ORACLE** where the comparison is meaningful (const-prop,
  folding, inlining, specialization, DCE, short-circuit — not machine-level
  codegen). Write the equivalent C++, `g++ -O3 -S`, and confirm MyLang folds
  what the compiler folds — or document *precisely* why not. The legitimate
  "why not"s are dynamic-typing soundness (`x+0`≠`x` since `x` may be a string;
  `false||5`≠`5` since `||` yields bool) and deliberately-out-of-scope passes
  (general algebraic simplification, loop unrolling, recursion folding). "It
  folds my example" is not the bar; "it folds what a compiler folds, or there's
  a written reason it can't" is.
- **State the PROPERTY, then test the property.** "A pure call with const args
  folds to a literal" is a property — so test a chain, a partial-const, a
  cross-input, a nested, and a side-effecting-operand case. If only the example
  works, the feature is unfinished: either generalize it or pin the limitation
  with a test that documents the current (limited) behavior, so the gap is
  *visible* and intentional rather than a latent surprise.

## Invariants & hazards (defense in depth)

This project deliberately builds many overlapping correctness layers (a
"Swiss-cheese" model: every check has blind spots, but stacked checks with
*different* blind spots make a bug clear them all very unlikely). The rules
below came out of a real, nasty bug — an MSVC-only, non-deterministic
wrong-result in cross-input REPL template instantiation, root-caused via CI
instrumentation (see `plans/function-templates.md`).

- **A red test on ANY platform is a real bug — never route around it.** A
  one-platform CI failure you can't reproduce locally is NOT "flakiness" to
  revert or disable the feature over; it is a defect to root-cause. "Can't
  reproduce locally" raises the instrumentation bar, it never lowers the bug's
  reality. Multi-compiler / multi-platform CI exists precisely to expose UB and
  logical-identity bugs the dev allocator hides — lean into it.

- **Never key a long-lived map by a raw AST-node pointer.** A `Construct *` is
  NOT a stable identity: the allocator recycles a freed node's *address*, so a
  map that outlives the node (e.g. across REPL inputs) can match a stale entry
  with a fresh node — a silent wrong-lookup, invisible to ASan (the memory is
  valid; the *identity* is wrong). Use a stable identity instead: the codebase's
  established ones are **`UniqueId *`** (interned names, never freed) and the
  **monotonic arenas** (`all_syms`/`all_funcs` — never truncated, only marked
  `pinned`). For nodes there is now **`Construct::node_id`** (a monotonic
  `uint64`; a clone gets a fresh one). The two remaining node-keyed maps
  (`id_sym`, `func_of_decl`, inferencer) are instead **scoped to one input**
  (cleared each `infer_input`) and re-resolved every pass, so no stale entry can
  survive — that combination is the fix for the bug above.

- **`ML_CHECK` / `ML_CHECK_MSG` (`defs.h`) are the assertion layer.** Use them —
  not bare `assert` — to state an invariant the code RELIES ON but a wrong
  change could break: "this is impossible if the code is correct." They must be
  **side-effect-free**. They follow the C `assert()` exactly: active unless
  `NDEBUG`, i.e. gated by the **`ASSERTS`** build flag (defaults **ON for every
  build type**, debug AND release, in both build systems — so every build and
  every CI lane exercises the full net). `ASSERTS=0` defines `NDEBUG`, compiling
  both the C asserts and these away — the way to measure their overhead
  (`make OPT=1 ASSERTS=0` vs `make OPT=1`). When ASSERTS is on, the build also
  enables stdlib container hardening (`_GLIBCXX_ASSERTIONS` / libc++
  `_LIBCPP_HARDENING_MODE`). **Assert the right things:** logical
  invariants the sanitizers CANNOT see — a union/tag mismatch (reading the wrong
  active member is valid memory, wrong meaning), a refcount underflow, a state
  the type system should have made impossible, a stable-identity check. Do NOT
  add asserts for plain out-of-bounds / shift-overflow / use-after-free —
  ASan/UBSan already catch those, and a real *runtime* condition (bad user
  input, I/O failure, a genuine type error) must `throw` a proper `Exception`,
  not assert. Examples in place: the `flat_*()` union-kind checks
  (`sharedarray.h`), `intrusive_ptr::release` refcount-underflow
  (`intrusiveptr.h`), `pod_get`/`pod_set` field validity (`structtype.h`),
  `Frame::init` `frame_size <= 64` (`eval.h`).

- **`RECYCLE=1` — the adversarial allocator.** `make RECYCLE=1 TESTS=1` builds a
  `Construct` allocator (a size-keyed LIFO free-list, `syntax.cpp`) that hands a
  just-freed node's address straight back to the next allocation, so any
  "AST pointer used as a stable identity" bug manifests DETERMINISTICALLY under
  `-rt` instead of depending on the host allocator's luck (ASan-poisons the
  free window too, so a dangling read is still caught). It is a general stress
  tool for that whole class and a future-regression net. **Honest scope:** it
  did *not* by itself reproduce the specific cross-input bug above — the REPL
  test path retains its ASTs, so there were no intra-test frees to recycle; the
  per-input map clear is the guard there. It runs as a dedicated CI lane
  (`linux.yml`, `-DRECYCLE=ON`) and a local matrix entry — not a replacement for
  the structural fixes.

- **CI maximizes correctness checks (it does not time anything).** Every CI lane
  builds with `ASSERTS` on (C asserts + `ML_CHECK` + stdlib container
  hardening), Debug lanes add ASan + UBSan, UBSan runs with
  `-fno-sanitize-recover` (a finding ABORTS, so it can't print-and-still-exit-0
  past the exit-code check), and there is a `RECYCLE=ON` lane. Slower is fine —
  performance is measured separately (`bench/`, a plain `make` release). Adding
  a new check here is cheap insurance; reach for it.

## Recipes

### Adding a builtin
1. Implement `EvalValue builtin_xxx(EvalContext *ctx, ExprList *exprList)` in
   the appropriate
   `src/builtins/*.cpp.h`. Builtins get **unevaluated** argument expressions —
   evaluate each yourself
   with `RValue(exprList->elems[i]->eval(ctx))`, and validate arity
   (`InvalidNumberOfArgsEx`) and
   types (`TypeErrorEx`), passing the argument's `start`/`end` `Loc`s for good
   error messages.
2. Register it in `types.cpp`: `make_const_builtin(...)` in the `const_builtins`
   map if it is pure and
   safe to run during const-eval, otherwise `make_builtin(...)` in the
   `builtins` map (runtime only —
   I/O, `rand`, mutation, `exit`, …). Const builtins are what const-folding is
   allowed to call.
3. Document it in `README.md` (const vs. non-const section) and add a test in
   `src/tests.cpp`.

**Memory safety with user callbacks.** A builtin that drives a sort/search with
a *user-supplied* callback must not assume the callback is well-behaved — it is
arbitrary script code. In particular `sort(arr, cmp)` (`builtins/arr.cpp.h`)
uses a **hand-rolled iterative heapsort**, not `std::sort`, for the
custom-comparator path: `std::sort`'s unguarded partition/insertion reads off
the ends of the buffer when the comparator isn't a strict weak ordering (a
heap-buffer-overflow reachable straight from a script), whereas the heapsort's
`sift_down` index strictly descends — so it terminates for *any* comparator —
and only ever indexes within `[0, n)`. It is hand-rolled rather than
`std::make_heap`/`std::sort_heap` because MSVC's debug STL wraps those in
comparator-validity instrumentation that *hangs* on a non-ordering comparator.
The default (no-comparator) path keeps `std::sort` — its `operator<` is a valid
ordering for homogeneous types and throws `TypeErrorEx` for incomparable ones.
Keep this distinction if you touch sorting or add another callback-driven
algorithm.

### Adding a value type
Touch all of: the `TypeE` enum (`type.h`) — mind the trivial/non-trivial
position vs. `t_str`; the
`TypeToEnum` specialization + `ValueU` union member (`evalvalue.h`); the
`TypeNames` and `AllTypes`
arrays (`types.cpp`, kept index-aligned with `TypeE`); and a new `TypeXxx` class
overriding the
needed virtuals in `src/types/xxx.cpp.h` (extend `TypeImpl<T>` for non-trivial
types to inherit the
type-erased lifecycle ops). Then `#include` the new `.cpp.h` in `types.cpp`.

### Adding an operator or keyword
Add to the `Op`/`Keyword` enum and the matching `OpString`/`KwString` array
(keep indices aligned),
wire it into the right `pExprNN` level (or `pStmt`) in `parser.cpp`, add the
`do_eval` behavior (a new
`Type` virtual for an operator, or a new node in
`syntax.h`/`syntax.cpp`+`eval.cpp` for a statement),
and cover it in `tests.cpp`. A **new `Construct` node must also implement
`clone()`** (pure virtual) — usually a few lines using the shared helpers;
omitting it is a compile error.

## Conventions

- **Incremental is fine; ending in a half-measure is not.** Landing a feature in
  stages — even with temporary duplication or a stubbed corner — is welcome, as
  long as the *task* ends at a proper solution. Don't stop at "works but
  duplicated/limited" and call it done; either finish the clean version or
  leave a written, tracked follow-up that says exactly what remains. A specific,
  non-negotiable instance: **the named-argument syntax must never cost an
  optimization.** A named call has to be optimized (const-fold, inline,
  specialize) *identically* to the positional call it desugars to — if a name
  ever disabled a fold that the positional form would get, the feature is a
  regression and would be better not used at all. Hold any sugar to the same
  bar: it may add spelling, never subtract capability.
- **Interactive `git rebase -i` is permitted in this repo** (the environment's
  general "no interactive flags" restriction is waived here by the maintainer) —
  use it to keep history clean / bisectable, e.g. squashing a fix into the
  commit that introduced the bug. Drive it non-interactively from an agent with
  `GIT_SEQUENCE_EDITOR` (rewrite the todo) and `GIT_EDITOR` (supply messages).
  `exp-work` is a topic branch whose history may be rewritten freely.
- **Never edit a source file while a build that compiles it is running.** A
  background `make`/`cmake` reads `src/` AND writes shared dep files (`.d/`) as
  it goes; editing during that window makes it compile a half-written file or
  corrupt a dep, producing a *bogus* "BUILD FAILED" that looks like a real
  regression and wastes a debugging cycle. Serialize: let a background build (or
  the test matrix) finish before touching sources, or run it in a separate
  `BUILD_DIR` / worktree. Docs (`CLAUDE.md`, `README.md`, plans) are not
  compiled, so editing those during a build is fine.
- **Every line stays within 80 columns** — code, comments, and the Markdown
  docs (`CLAUDE.md` included). Wrap long expressions; put a comment that would
  overflow on its own line above the code instead of trailing it. (A few legacy
  files predate this and still have long lines; hold new or edited code to 80.)
- C++17, `-Wall -Wextra -Wno-unused-parameter`, compiled with `-fwrapv` (signed
  overflow wraps — and
  is *relied upon*; don't "fix" wrap-dependent arithmetic).
- Every file starts with `/* SPDX-License-Identifier: BSD-2-Clause */`.
- Core typedefs (`defs.h`): `int_type = intptr_t`, `float_type = double`
  (printf/snprintf with `%f`/`%.*f`; the comment warns to update the format
  strings and math builtins if you change it). `double` (not `long double`)
  keeps `EvalValue` small — long double's 16-byte alignment padded it from 40
  to 48 bytes, inflating array memory traffic and every value copy — matches
  Python's float, and uses the faster double libm. `size_type = uint32_t`
  (`size_t` on MSVC).
- No third-party dependencies, ever — that's a hard design constraint of the
  project, including for the
  test harness. Don't introduce one.
- When implementation behavior and the README disagree, treat it as a bug to
  surface, not a silent
  choice to make — the README is the spec.
