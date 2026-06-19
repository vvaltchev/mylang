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
overflow is *defined* here, not a bug). `-fno-omit-frame-pointer` is added
whenever either sanitizer is on. `-rt` is verified green under both.

CMake is also supported (used by CI):
`cmake -DTESTS=1 -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .`
(`-DGCOV=ON` for a coverage build, GCC only). It enables LTO via
`INTERPROCEDURAL_OPTIMIZATION` for the `Release`/`RelWithDebInfo` configs (so
Debug and coverage builds are untouched), portable across GCC/clang/MSVC;
configure with `-DLTO=OFF` to disable. The same `ASAN`/`UBSAN` options exist
(`-DASAN=ON/OFF`), defaulting **on for a `Debug` build** and off otherwise — the
analog of `OPT=0` vs `OPT=1` — and are applied on GCC/clang only (skipped on
MSVC). CMake now also passes `-fwrapv` (non-MSVC), matching the Makefile so the
relied-upon signed wraparound is defined in CMake builds too. CI
(`.github/workflows/`) builds Debug+Release × g+++clang
on Linux, plus macOS and Windows, and runs `./mylang -rt`.

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
```
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

**Only `src/*.cpp` are compiled** (the Makefile globs them) — eleven translation
units:
`lexer.cpp`, `parser.cpp`, `syntax.cpp`, `resolver.cpp`, `inferencer.cpp`,
`eval.cpp`, `types.cpp`, `stype.cpp`, `backtrace.cpp`, `mylang.cpp`,
`tests.cpp`.

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
  helpers).
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

### Lexer

Produces a `vector<Tok>`. A `Tok` is `{ TokType, Loc, value/op/kw }` where
`TokType` ∈ {integer, id,
op, kw, str, floatnum, unknown}. Operators (`Op` enum, `operators.h`) and
keywords (`Keyword` enum,
`lexer.h`) are recognized via `std::map`s built once from the `OpString` /
`KwString` arrays — keep
those arrays index-aligned with the enums if you add tokens. `invalid_tok` is
the EOF sentinel.
Chars are 8-bit; **no Unicode** (deliberate, to stay small).

### Parser — operator-precedence ladder

Binary-operator precedence is encoded as a ladder of parse functions
`pExpr01 … pExpr14`, each
producing a matching AST node class `Expr01 … Expr14`. **The numbering has
gaps** — only
`02,03,04,06,07,11,12,14` carry real operators (levels with no operators in this
language are
skipped):

- `pExpr01` — primaries + postfix chains: literals, `()`, `[...]` array, `{...}`
  dict, identifiers, then any run of call `(...)`, subscript/slice `[...]`,
  member `.id`
- `pExpr02` — unary `+ - !` (right-recursive, so `!!x`, `-+x` work)
- `pExpr03` — `* / %`
- `pExpr04` — `+ -`
- `pExpr06` — `< > <= >=`
- `pExpr07` — `== !=`
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
`is_const` (a const global/builtin/explicit-pure func) and it nests no function.
Conservative: self-recursion and calls to *other* auto-pure (non-explicit) funcs
are not recognized. `FuncDeclStmt::{explicit_pure, effective_pure}` back the
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
`$specN` is registered (deduped by (func, const-arg tuple) — an array/dict keyed
by its `intptr` identity in `value_repr`, so the same const object shares one
clone — inserted at the root block's front) and the call redirected; the clone
keeps the same frame (no re-resolution) and a `FuncDeclStmt::display_name` makes
backtraces show the original name, not `$specN`. A **tail call to a block-bodied
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
leaves the tree untouched for the later passes. Full design + the decisions
behind it: `plans/type-inference.md`, `plans/type-inference-questions.md`.

- **Static types** are `STy` (`stype.h`), distinct from the runtime `Type *`:
  `None` (the only-none / not-yet-pinned unit), `Int`, `Float`, `Str`,
  `Array<elem>`, `Dict<k,v>`, `Func(params)->ret`, `Exception`, `Dyn` (explicit
  top), each with an `opt` (nullable) flag. The lattice ops are
  `assignable`/`join`/`unify`/`equal` (`int <= float` promotion; `None`/`opt`
  nullability; mixed container elements fall to `Dyn`; a scalar/kind conflict is
  an error).
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
  nullability is *declared* (`opt`); local/return nullability is *inferred*
  (`None` joined with a concrete `T` is `opt T`). `runtime(x)` returns `Dyn`
  (its documented opt-out: it defers to runtime). `==`/`!=` are always
  well-typed (→ int); ordering is numeric-or-string. `str + anything` → str.
  Higher-order builtins (`map(func,c)`, `filter(func,c)`, `sort(c,func)`,...)
  feed the container's element type into the callback's params (named **or**
  inline lambda; `callee_funcinfo`). A `var f = <lambda>` binds `f`'s `TypeSym`
  to the lambda's `FuncInfo`, so calls to `f` type its params and check arity.
- **New surface syntax**: the `opt` and `dyn` keywords, usable as modifiers on a
  parameter (`func f(opt x, dyn y)`) or a var/const decl (`var dyn z = ...;`,
  `var opt w;`). `opt` = nullable (may hold `none`); `dyn` = dynamically typed
  (behaves as today; inference does not constrain it — the escape hatch for
  genuinely polymorphic code). Implemented as `Identifier::{opt_mod,dyn_mod}`
  (params, via `pFuncParam`) and `pFlags::{pInOptDecl,pInDynDecl}` (decls).
- **Errors** are compile-time (`DECL`-style plain `Exception`s, **not**
  `RuntimeException`s, so script `try/catch` cannot catch them; `errors.h`):
  `TypeMismatchEx` (type change / bad operator / wrong arg type / not callable),
  `NullabilityEx` (`none`/`opt` used where a non-opt value is required),
  `WrongArgCountEx` (arity). Each carries an interned custom message + a `Loc`.
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
it proved is a non-null int/float. After `resolve_names`, `specialize_types`
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
unboxed path via `eval_cond` when the condition is a known int. The specializer
recurses bottom-up so nested typed subtrees chain `eval_int` calls with no
boxing between them. **Effect:** ~2.8x on `bench/44_primes_sqrt`, ~2x on
float-heavy reductions; the once-slower-than-Python primes benchmark is now
faster. `th` is copied by `copy_base_fields` (clones/inliner preserve it), and
the typed eval's `get<int_type>()` throws `TypeError` if inference were ever
wrong (a safety net, not silent corruption). See `plans/type-inference.md` M8.

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
- **Mixed int/float promotion is centralized in `num_bin_op()`**
  (`evalvalue.h`), not in the type
  classes. The type virtuals are single-type: `TypeInt::add` only handles an int
  RHS, `TypeFloat::add`
  also accepts an int RHS (promoting it). `num_bin_op(a, b, &Type::op)` is the
  dispatch chokepoint:
  when `a` is int and `b` is float it promotes `a` to float first, so
  `int OP float` lands in
  `TypeFloat` and behaves identically to `float OP int`. **Dispatch binary
  arithmetic/comparison
  through `num_bin_op`, never by calling `a.get_type()->add(...)` directly** —
  every call site does
  (the `ExprNN::do_eval` ladder and compound-assign in `eval.cpp`, `EvalValue`'s
  `== != < <= > >=`
  operators, `builtin_sum`). It is a no-op for any non-`(int,float)` operand
  pair, so string/array/etc.
  comparisons pass through unchanged. (Logical `&&`/`||` and unary ops are *not*
  routed through it.)
  Note for dict keys: an integer-valued float hashes as the equal int
  (`TypeFloat::hash`) so that
  `1` and `1.0`, which compare equal, are the same key.
- **The trivial / non-trivial boundary is `t_str`.** `TypeE` order matters:
  `t_none, t_lval, t_undefid, t_int, t_builtin, t_float` (`< t_str`, trivial,
  stored inline,
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
- **`d.key` (member access) auto-vivifies.** `MemberExpr::do_eval` `emplace`s
  the key with `none` if
  absent and returns it as an assignable `LValue` — so reading a missing member
  in lvalue position
  *mutates* the dict. Keep this in mind when reasoning about dict side effects.

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
- **Flat (unboxed) int/float storage.** `SharedObject` carries a `Storage kind`
  (`general`/`ints`/`floats`) and an **anonymous union** of `vec` (the
  `vector<LValue>`, 48-byte slots), `ivec` (`vector<int_type>`), and `fvec`
  (`vector<float_type>`) — the latter two are 8-byte unboxed slots, so a
  homogeneous int/float array moves ~6× less memory in bulk ops. A `union`
  member can't have non-trivial ctors/dtors, so `SharedObject` placement-news the
  live member per kind and the dtor switches on `kind`. Flat storage is produced
  by `range()`, `array(N, value)` (value-driven: an int/float fill picks
  `ints`/`floats`, else general; `array(N)` with no fill stays general `none`),
  and `make_array(N, gen)` (optimistic-flat by the callback's result kind). The
  flat fast paths (each branches on `skind()`, reads `flat_ints()`/
  `flat_floats()` directly, no promotion): `sum`/`reverse`/`sort`
  (no-comparator)/`min`/`max`/`append`/`pop`/`foreach`,
  `TypeArr::{subscript (rvalue read),
  to_string, eq, add}`, `Subscript::eval_int`/`eval_float`, and the flat
  subscript-store `try_flat_subscript_store` (`eval.cpp`: `a[i] = v` /
  `a[i] OP= v` writes the scalar straight into the flat vector — gated on a
  side-effect-free id base so the general fall-through can re-eval it; a
  non-fitting element type promotes). `clone_internal_vec`, `make_const_clone`,
  and `clone_to_mutable` are kind-aware so clone/COW/const keep flat. **Anything
  without a flat fast path calls `get_vec()`, which `promote_to_general()`s in
  place (flat → `vector<LValue>`, value-preserving, sound even when shared/
  sliced) and reuses the general code** — so the feature is incremental and
  correct by construction. `size()` is kind-aware and does *not* promote.
  `array_storage(a)` reports `"ints"`/`"floats"`/`"general"` (tests pin it).
  **Representation is value-driven, NOT type-driven** (the inferred type does
  not gate it yet): a flat array whose true static type is `array<dyn>` (e.g. a
  mixed `append` widened it via `contribute_container`) is still made flat, then
  promotes on the non-fitting element — a deferred refinement. **Gotcha:** any
  pass that inspects a const array's element type must read it from `skind()`,
  not `get_view()`/`get_vec()` — those promote the const value (this bit the
  inferencer's `sty_from_value`, which had promoted every const array and
  defeated the whole optimization). See `plans/typed-arrays.md` (approach B);
  remaining: float-array producers (M2), the `array(N)` 1-arg type-driven
  default, and flat `insert`/`erase`/`map`/`filter`.
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

## Error model

`errors.h` defines an `Exception` base (`name`, `msg`, `loc_start`, `loc_end`)
and two macros:

- `DECL_SIMPLE_EX` — parse-time / internal errors (`SyntaxErrorEx`,
  `InternalErrorEx`,
  `CannotRebindConstEx`, `ExpressionIsNotConstEx`, …). **Not catchable** from
  script.
- **Compile-time type errors** (`TypeMismatchEx`, `NullabilityEx`,
  `WrongArgCountEx`) — thrown by the type inferencer (see "Static type
  inference"). Plain `Exception`s (not `RuntimeException`s), so **not catchable**
  from script; each carries a custom interned message + `Loc`. A statically
  provable type error is reported here, before the program runs.
- `DECL_RUNTIME_EX` — subclasses of `RuntimeException` (adds `clone()` +
  `[[noreturn]] rethrow()`):
  `DivisionByZeroEx`, `TypeErrorEx`, `OutOfBoundsEx`, `NotLValueEx`,
  `NotCallableEx`,
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

- **Interactive `git rebase -i` is permitted in this repo** (the environment's
  general "no interactive flags" restriction is waived here by the maintainer) —
  use it to keep history clean / bisectable, e.g. squashing a fix into the
  commit that introduced the bug. Drive it non-interactively from an agent with
  `GIT_SEQUENCE_EDITOR` (rewrite the todo) and `GIT_EDITOR` (supply messages).
  `exp-work` is a topic branch whose history may be rewritten freely.
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
