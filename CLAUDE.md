# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **READ `README.md` IN FULL BEFORE TOUCHING ANYTHING.** This is a small project and the README is
> the complete language specification (every keyword, every builtin, every semantic rule, with
> examples). It is not optional reference material to consult on demand — read the whole thing up
> front, once, so you know the language the interpreter implements. This CLAUDE.md covers the *C++
> implementation*; the README covers the *language*. You need both in your head before making changes.

> **KEEP THE DOCS IN SYNC WITH THE SOURCE — IN THE SAME CHANGE.** `README.md` and this `CLAUDE.md`
> are part of the codebase, not afterthoughts. Any code change must carry its documentation update in
> the *same commit*, never as a follow-up:
> - **`README.md`** whenever script-visible behavior changes — a new/removed/renamed keyword, builtin,
>   operator, or numeric constant; changed semantics; new error conditions. README is the language
>   spec; if it and the interpreter disagree, that's a bug.
> - **`CLAUDE.md`** whenever the *implementation* shape changes — a new source file or `.cpp.h`, a new
>   `TypeE`/AST node, a changed design rule or invariant, a new convention, or anything that would make
>   a sentence in this file wrong. After editing code, reread the relevant CLAUDE.md section and fix any
>   statement the change just falsified.
>
> A change that alters behavior or architecture but leaves the docs stale is incomplete.

## What this is

MyLang is an educational, dynamically-typed scripting language (C-looking syntax, Python-ish
semantics) implemented as a tree-walking interpreter in portable C++17. It has **no dependencies**
beyond the standard library, including for its tests. The single `mylang` executable both compiles
(lex + parse + const-fold) and runs scripts. Author's goal was to have fun writing a recursive-descent
parser; correctness and clarity matter more than raw speed, though performance shaped several core
design choices (see the value model below).

## Build & run

```
make -j                    # release build (-O3) -> build/mylang
make -j TESTS=1 OPT=0      # debug build with unit tests compiled in (needed for -rt)
make -j BUILD_DIR=other    # out-of-tree build
make clean
```

`OPT` defaults to 1 (`-O3`); `OPT=0` drops it. `TESTS=1` adds `-DTESTS`, which is what compiles the
`-rt` suite into the binary. Base flags: `-std=c++17 -ggdb -Wall -Wextra -Wno-unused-parameter
-fwrapv`. The Makefile auto-generates header dependencies under `.d/`.

CMake is also supported (used by CI): `cmake -DTESTS=1 -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .`
(`-DGCOV=ON` for a coverage build, GCC only). CI (`.github/workflows/`) builds Debug+Release × g+++clang
on Linux, plus macOS and Windows, and runs `./mylang -rt`.

Running scripts:
```
./build/mylang FILE              # run a script (extra args become the script's `argv` array)
./build/mylang -e 'EXPR'         # run inline source (everything after -e is concatenated)
./build/mylang -s FILE           # dump the syntax tree (shows const-folding results) then run
./build/mylang -t FILE           # dump tokens
./build/mylang -nc FILE          # disable const-evaluation (debug; compare -s output with/without)
./build/mylang -nr FILE          # parse/validate only, don't run
```
`-s` / `-nc` are the two indispensable debugging tools: `-s` shows you exactly what survived
const-folding, and `-nc` lets you see the tree as written before folding. Reach for them whenever
behavior surprises you.

## Tests

```
./build/mylang -rt               # run the whole suite (requires a TESTS=1 build)
./build/mylang -rt -s            # same, but dump the syntax tree of any failing test
```

Tests are **not** a separate framework — they are entries in the `static const std::vector<test>
tests` table in `src/tests.cpp`. Each entry is `{ "name", { "source line", "line", ... }, optional
&typeid(ExpectedEx) }`. `check()` lexes+parses+evals the joined source lines. A test **passes** if it
throws nothing — or, when an expected exception type is given, throws *exactly* that type (compared via
`&typeid(e) != t.ex`). There is no single-test CLI selector; `-rt` runs all of them and `exit(1)`s if
any fail. Add a test by appending an entry to that table (no registration needed). Note the expected
exception is matched against the *static* C++ type; user-level `throw ex("Foo")` always surfaces as
`ExceptionObject` (a.k.a. `DynamicExceptionEx`), not a distinct C++ type.

## Source layout & compilation model

**Only `src/*.cpp` are compiled** (the Makefile globs them) — seven translation units:
`lexer.cpp`, `parser.cpp`, `syntax.cpp`, `eval.cpp`, `types.cpp`, `mylang.cpp`, `tests.cpp`.

- `mylang.cpp` — CLI entry point, arg parsing, the top-level `try/catch` that turns thrown
  `Exception`s into formatted error output (`dumpLocInError` prints the source line + a `^` caret).
- `lexer.cpp` / `lexer.h` — `lexer(line, line_no, tokens)` appends tokens for one source line. The
  CLI feeds it line by line so token `Loc`s carry real line/column numbers.
- `parser.cpp` / `parser.h` — recursive-descent parser, const-folding woven in. `pBlock()` is the
  entry point.
- `syntax.h` / `syntax.cpp` — the `Construct` AST node hierarchy and its `serialize()` (what `-s`
  prints).
- `eval.cpp` — the `do_eval()` bodies: the actual tree-walking interpreter.
- `types.cpp` — the single TU that stitches the type system and builtins together (see next section).

**The `.cpp.h` convention.** Files under `src/types/` and `src/builtins/` are named `*.cpp.h` and are
`#include`d *once* into `types.cpp`. Each starts with a comment: "this is NOT a header file… it's a
C++ file in the form of a header, just because it's faster to compile it this way." So `types.cpp` is
the one TU that compiles every `TypeXxx` class and every `builtin_xxx`. When adding type or builtin
code, put it in the matching `.cpp.h` and rely on it being pulled into `types.cpp` — it will not
compile standalone, and there is nothing to add to the Makefile.

**Why so many headers are templates.** `type.h` (`TypeTemplate`), `sharedarray.h`
(`SharedArrayObjTempl`), `shareddict.h`, `exceptionobj.h` are templated *not* for genericity but to
break a circular include order: they need `EvalValue`/`LValue`, which need them. `evalvalue.h`
instantiates them with concrete types via typedefs (`typedef TypeTemplate<EvalValue> Type;`,
`typedef SharedArrayObjTempl<LValue> SharedArrayObj;`, …). Treat these typedefs as the real types.

## The pipeline

**lexer → recursive-descent parser (with const-folding woven in) → tree-walking evaluator.**

### Lexer

Produces a `vector<Tok>`. A `Tok` is `{ TokType, Loc, value/op/kw }` where `TokType` ∈ {integer, id,
op, kw, str, floatnum, unknown}. Operators (`Op` enum, `operators.h`) and keywords (`Keyword` enum,
`lexer.h`) are recognized via `std::map`s built once from the `OpString` / `KwString` arrays — keep
those arrays index-aligned with the enums if you add tokens. `invalid_tok` is the EOF sentinel.
Chars are 8-bit; **no Unicode** (deliberate, to stay small).

### Parser — operator-precedence ladder

Binary-operator precedence is encoded as a ladder of parse functions `pExpr01 … pExpr14`, each
producing a matching AST node class `Expr01 … Expr14`. **The numbering has gaps** — only
`02,03,04,06,07,11,12,14` carry real operators (levels with no operators in this language are
skipped):

| fn / node | operators |
|-----------|-----------|
| `pExpr01` | primaries + postfix chains: literals, `()`, `[...]` array, `{...}` dict, identifiers, then any run of call `(...)`, subscript/slice `[...]`, member `.id` |
| `pExpr02` | unary `+ - !` (right-recursive, so `!!x`, `-+x` work) |
| `pExpr03` | `* / %` |
| `pExpr04` | `+ -` |
| `pExpr06` | `< > <= >=` |
| `pExpr07` | `== !=` |
| `pExpr11` | `&&` |
| `pExpr12` | `\|\|` |
| `pExpr14` | assignment `=  +=  -=  *=  /=  %=`, plus `var`/`const` decls and id-list targets |

`pExprGeneric<ExprT>` implements the common left-associative chain: it collects `(Op, operand)` pairs
into a `MultiOpConstruct`, evaluated left-to-right in `do_eval` by mutating an accumulator
(`val.get_type()->add(val, rhs)`, etc.). `pBlock` is the entry point; `pStmt` dispatches statements
(`if`/`while`/`for`/`foreach`/`func`/`try`/`throw`/`return`/braced block/expression-statement).

A `fl` bitmask of `pFlags` (`pInDecl`, `pInConstDecl`, `pInLoop`, `pInStmt`, `pInFuncBody`,
`pInCatchBody`) is threaded through every parse function and gates legality: `break`/`continue` only
under `pInLoop`, `return` only under `pInFuncBody`, `rethrow` only under `pInCatchBody`, and
`var`/`const` set `pInDecl`/`pInConstDecl` so `pExpr14` knows to *declare* rather than *assign*.

### Const-evaluation — the central design feature

Constants are evaluated **at parse time**, like C++ `constexpr`. This is the project's defining trait
and it lives *inside the parser*. Mechanics:

- `ParseContext` owns a chain of **const `EvalContext`s** (`const_ctx`). `pBlock` pushes a fresh
  nested const ctx on entry and pops it on exit, so the const scope chain mirrors lexical scope.
- Every `Construct` carries an `is_const` flag, propagated bottom-up: a node is const iff all its
  children are const. String literals are const; `var`/freshly-declared identifiers are not.
- As soon as the parser finishes a const node, it evaluates it against `const_ctx` and calls
  **`MakeConstructFromConstVal()`** to replace the subtree with a literal node. That function inlines
  `int`/`float`/`none`/`str` unconditionally, and `arr`/`dict` only when `process_arrays` is set.
- **Scalars vs. containers (`ShouldConstSymbolExistAtRuntime`).** Const scalars are inlined
  everywhere and their declaration is dropped from the runtime AST entirely (the decl parses to a
  `NopConstruct`, which `pBlock` discards) — `-s` shows them simply gone. Const **arrays, dicts, and
  funcs** are *kept* as real runtime symbols (declared once), because inlining a million-element array
  at every use site would be wasteful; operations on them like `arr[2]` or `len(arr)` still get
  const-folded to literals though.
- **Statement folding:** an `if` with a const condition is replaced by just its taken branch; a
  `while`/`foreach` proven to never execute (const-false condition / const-empty container) is dropped.
- **`pure func`s are the escape hatch.** `pure func f(...)` parses with `is_const = true` and is
  registered into `const_ctx`, so it *can* be invoked during const-eval (a plain `func` cannot). Pure
  funcs may not have a capture list and see only consts + their own params. A `CallExpr` folds when
  the callee and all args are const — that's how `sort(arr, pure func(a,b) => a<b)` runs at parse time.
- **Early failure:** exceptions raised *during* const-eval propagate immediately and are *not*
  catchable by script `try/catch` (the parser never enters a const assignment inside a try). This is
  intentional — const errors should fail the build, not be swallowed.
- A const decl in `pInConstDecl` whose rvalue isn't actually const throws `ExpressionIsNotConstEx`.

`-s` (with vs. without `-nc`) is the way to *see* all of the above happening.

## The value & type model (the subtle part)

- **`EvalValue`** (`evalvalue.h`) is a hand-rolled tagged union: a `ValueU` union plus a `Type *type`
  tag. It deliberately avoids `std::variant` — the comment in `flatval.h` records that `std::variant`
  made the whole interpreter ~50% slower on a simple loop. `size_type` is `uint32_t` (not `size_t`)
  specifically to keep `EvalValue` small and fast to copy.
- **`FlatVal<T>`** (`flatval.h`) is an `alignas(T) char[sizeof(T)]` buffer with placement-new ctors.
  It's what lets non-trivial C++ objects (`SharedStr`, `shared_ptr<…>`, `SharedArrayObj`) live inside
  the union. Because a union can't run their ctors/dtors, `EvalValue`'s copy/move/destroy route
  through **type-erased ops** (`TypeErasureOps` in `type.h`: `default_ctor`/`dtor`/`copy_ctor`/
  `move_ctor`/`copy_assign`/`move_assign`), which `TypeImpl<T>` (in `evaltypes.cpp.h`) implements via
  placement-new + `reinterpret_cast`.
- **`Type`** is a polymorphic operations table, **one singleton per kind**, held in the global
  `AllTypes` array (`types.cpp`) indexed by the `Type::TypeE` enum (`type.h`). Every operation —
  `add`, `sub`, `mul`, `lt`, `eq`, `is_true`, `to_string`, `len`, `subscript`, `slice`, `clone`,
  `hash`, `use_count`, `intptr`, … — is a `virtual` on `TypeTemplate` dispatched through the value's
  `type` pointer. The base implementations throw `TypeErrorEx`; a type "gains" a behavior purely by
  overriding the relevant virtual (see `src/types/int.cpp.h` for the canonical example). Binary ops
  **mutate the left operand in place** (`void add(EvalValue &a, const EvalValue &b)` does `a += b`).
- **The trivial / non-trivial boundary is `t_str`.** `TypeE` order matters:
  `t_none, t_lval, t_undefid, t_int, t_builtin, t_float` (`< t_str`, trivial, stored inline,
  bit-copyable) then `t_str, t_func, t_arr, t_ex, t_dict` (`>= t_str`, non-trivial, need the
  type-erased lifecycle ops). Hot paths branch on `type->t < Type::t_str` / `>= Type::t_str` (e.g.
  `EvalValue::clone()` short-circuits for trivial types). If you add a type, its position relative to
  `t_str` decides which machinery applies.
- `t_lval` and `t_undefid` are **internal pseudo-types**, never visible to scripts (blank entries in
  `TypeNames`). They tag the two special `EvalValue` payloads below.
- **`LValue`** (`evalvalue.h`) = an assignable slot: an `EvalValue` + `is_const` flag + an optional
  back-pointer (`container`, `container_idx`) used when the slot is an *element of an array* (needed
  for copy-on-write, below).
- **`RValue(v)`** collapses an `EvalValue` holding an `LValue *` down to the contained value, and
  throws `UndefinedVariableEx` if it holds an `UndefinedId`. Builtins and operators call `RValue(...)`
  on every operand. `Identifier::do_eval` returns an `EvalValue` wrapping `LValue *` when the symbol
  is found (walking the parent chain), else an `UndefinedId{name}` sentinel — *not* an immediate
  error, which is what lets `defined()` and declaration-vs-assignment logic work.
- **`EvalContext`** (`eval.h`) is a lexical scope: `map<const UniqueId *, LValue>` + `parent` pointer
  + `const_ctx`/`func_ctx` flags. The root context auto-loads `const_builtins` (always) and `builtins`
  (only when not a const ctx). Each `Block` evaluates in a fresh child `EvalContext`.
- **`UniqueId`** (`uniqueid.h`) interns identifier strings in a global `std::set`; symbols are keyed
  by the interned *pointer*, so lookup is pointer comparison. (Global mutable state lives in
  `types.cpp`: `UniqueId::unique_set`, `EvalContext::builtins`, `AllTypes`, `empty_str/empty_arr/none`.)

## Evaluation specifics worth knowing before editing `eval.cpp`

- **Control flow is implemented with C++ exceptions** (defined locally in `eval.cpp`): `LoopBreakEx`,
  `LoopContinueEx`, `ReturnEx`, `RethrowEx`. Loops catch break/continue; `continue` *must* unwind via
  exception because it can fire from inside arbitrarily nested `if`s. `do_func_call` has an
  optimization: a top-level `return` statement is evaluated directly without throwing `ReturnEx`.
- **`Construct::eval()` wraps `do_eval()`** to attach the node's source `Loc` to any in-flight
  `Exception` that doesn't already carry one — this is how runtime errors get pointed at source.
  Override `do_eval`, not `eval`.
- **Function call scoping is lexical/closure-based.** `do_func_call` binds params into an
  `args_ctx` whose parent is the function's **`capture_ctx`**, not the call site. A `FuncObject` holds
  the `FuncDeclStmt *` plus that `capture_ctx`, which is parented to the *root* context — so functions
  cannot see caller locals or globals, only captures (and, for pure funcs, only consts + params).
  Builtins are different: they receive the **caller's `ctx`** and the **unevaluated** `ExprList`.
- **`clone()` semantics differ by capture.** A non-capturing `FuncObject` clones to *itself* (shared
  `shared_ptr`); a capturing one is deep-copied so each clone has independent captured state. This is
  the mechanism behind the counter/closure examples in the README.
- **Multiple assignment & array expansion** all funnel through `Expr14` + `handle_single_expr14`:
  an `IdList` lvalue with an array rvalue spreads element-wise (`var a,b = [1,2]`), with a non-array
  rvalue assigns the same value to each (`var a,b = 0`). The same helper drives `foreach`
  tuple-unpacking and the `indexed` keyword.
- **`d.key` (member access) auto-vivifies.** `MemberExpr::do_eval` `emplace`s the key with `none` if
  absent and returns it as an assignable `LValue` — so reading a missing member in lvalue position
  *mutates* the dict. Keep this in mind when reasoning about dict side effects.

## Copy-on-write containers

Strings, arrays, and dicts are reference-counted with value semantics preserved via COW:

- **`SharedStr`** (`sharedstr.h`): immutable `shared_ptr<string>` + `off`/`len` slice view. Slices are
  cheap views; strings are never mutated in place. Copies are forbidden (`= delete`), only moves —
  enforcing the no-accidental-copy intent.
- **`SharedArrayObj`** (`sharedarray.h`): `shared_ptr<SharedObject{ vec, set<live slices> }>` +
  `off`/`len`/`slice`. A slice registers itself in the parent's `slices` set (and unregisters on
  move/destroy). Writing through an array-element `LValue` (`LValue::get_value_for_put` in `eval.cpp`)
  triggers COW: if the container is a slice, or is aliased (`use_count > 1` / has live slices), it is
  cloned first so the write doesn't bleed across logically-distinct arrays.
- **`DictObject`** (`shareddict.h`) is analogous (`shared_ptr` wrapper).
- The non-const `intptr(symbol)` builtin exposes the underlying object pointer; the test suite uses it
  to assert exactly when two slices do/don't share storage. If you change COW logic, those tests are
  your spec.

## Error model

`errors.h` defines an `Exception` base (`name`, `msg`, `loc_start`, `loc_end`) and two macros:

- `DECL_SIMPLE_EX` — parse-time / internal errors (`SyntaxErrorEx`, `InternalErrorEx`,
  `CannotRebindConstEx`, `ExpressionIsNotConstEx`, …). **Not catchable** from script.
- `DECL_RUNTIME_EX` — subclasses of `RuntimeException` (adds `clone()` + `[[noreturn]] rethrow()`):
  `DivisionByZeroEx`, `TypeErrorEx`, `OutOfBoundsEx`, `NotLValueEx`, `NotCallableEx`,
  `AssertionFailureEx`, `CannotOpenFileEx`, `InvalidValueEx`. These are the ones script `try/catch`
  can handle (matched by name).
- **User exceptions** (`ex("Name", payload)`) are `ExceptionObjectTempl` (`exceptionobj.h`), a
  `RuntimeException` whose C++ type name is `"DynamicExceptionEx"` but whose *script-visible* name is
  the dynamic `dyn_name`. `try/catch` in `eval.cpp` (`do_catch`) clones the in-flight exception, then
  matches catch clauses against that dynamic name; `finally` runs via a scope guard; `rethrow`
  re-throws the saved exception with the rethrow site's `Loc`.
- Always pass `Loc start, end` to thrown exceptions where you can, so `mylang.cpp` can render the caret.

## Recipes

### Adding a builtin
1. Implement `EvalValue builtin_xxx(EvalContext *ctx, ExprList *exprList)` in the appropriate
   `src/builtins/*.cpp.h`. Builtins get **unevaluated** argument expressions — evaluate each yourself
   with `RValue(exprList->elems[i]->eval(ctx))`, and validate arity (`InvalidNumberOfArgsEx`) and
   types (`TypeErrorEx`), passing the argument's `start`/`end` `Loc`s for good error messages.
2. Register it in `types.cpp`: `make_const_builtin(...)` in the `const_builtins` map if it is pure and
   safe to run during const-eval, otherwise `make_builtin(...)` in the `builtins` map (runtime only —
   I/O, `rand`, mutation, `exit`, …). Const builtins are what const-folding is allowed to call.
3. Document it in `README.md` (const vs. non-const section) and add a test in `src/tests.cpp`.

### Adding a value type
Touch all of: the `TypeE` enum (`type.h`) — mind the trivial/non-trivial position vs. `t_str`; the
`TypeToEnum` specialization + `ValueU` union member (`evalvalue.h`); the `TypeNames` and `AllTypes`
arrays (`types.cpp`, kept index-aligned with `TypeE`); and a new `TypeXxx` class overriding the
needed virtuals in `src/types/xxx.cpp.h` (extend `TypeImpl<T>` for non-trivial types to inherit the
type-erased lifecycle ops). Then `#include` the new `.cpp.h` in `types.cpp`.

### Adding an operator or keyword
Add to the `Op`/`Keyword` enum and the matching `OpString`/`KwString` array (keep indices aligned),
wire it into the right `pExprNN` level (or `pStmt`) in `parser.cpp`, add the `do_eval` behavior (a new
`Type` virtual for an operator, or a new node in `syntax.h`/`syntax.cpp`+`eval.cpp` for a statement),
and cover it in `tests.cpp`.

## Conventions

- C++17, `-Wall -Wextra -Wno-unused-parameter`, compiled with `-fwrapv` (signed overflow wraps — and
  is *relied upon*; don't "fix" wrap-dependent arithmetic).
- Every file starts with `/* SPDX-License-Identifier: BSD-2-Clause */`.
- Core typedefs (`defs.h`): `int_type = intptr_t`, `float_type = long double` (printf with `%Lf` — the
  comment warns to update format strings if you change it), `size_type = uint32_t` (`size_t` on MSVC).
- No third-party dependencies, ever — that's a hard design constraint of the project, including for the
  test harness. Don't introduce one.
- When implementation behavior and the README disagree, treat it as a bug to surface, not a silent
  choice to make — the README is the spec.
