# Total type inference for MyLang (tree-walker)

Status: **planning.** This is a large, multi-milestone feature. This document is
the design + task order; nothing is implemented yet. Read it in full before
touching code, and update it as decisions firm up.

## 1. Goal & non-goals

**Goal.** A whole-program, multi-pass static type-inference pass over the AST
that gives **every** variable, const, parameter, and function return a single
fixed static type, inferred (no type-annotation syntax required). The pass runs
at compile time (after parsing/resolution, before execution) and **rejects**
programs that are not type-sound:

- a variable/const that would hold two incompatible concrete types on different
  code paths (a "type change") — error;
- `none` flowing into a non-optional position — error;
- an operator/call/subscript applied to an incompatible type — error;
- a call with the wrong arity or argument types — error.

Whole-program is what makes this possible: the entire source is one file,
nothing escapes to other translation units, so the type of everything is
knowable — *eventually*. The only genuine unknown is "we have not seen a
concrete use yet" (e.g. `var x = none;` or `func f(x) => x;` never called with a
concrete type). Those resolve to a special `None` type and **that's fine** — the
program is still sound, the value is simply only-ever-`none`.

**Why this needs to be multi-pass (not a single forward walk).** A statement's
types can be pinned by statements that come *after* it: a forward reference to a
function whose return type depends on a later definition; a parameter whose type
is fixed only by a later call site; a `var x = none;` that a later `x = 5;`
pins to `int`. So we collect constraints from the whole tree first, then solve
them to a fixpoint. (This is the user's exact intuition: "statements after a
given one should be able to make the interpreter infer the type of a given
statement.")

**Non-goals (this plan).**
- Type-annotation *syntax* (`func f(int x) -> int`). Inference only; the only new
  surface syntax is the `opt` and `dyn` modifiers (see §4, D5).
- The *speed* payoff (typed/monomorphic AST nodes). The inference pass produces
  type annotations on nodes; *exploiting* them for speed is a separate follow-on
  phase, sketched in §9. This plan delivers inference + checking + annotation.
- Flow-sensitive nullability narrowing (smart-casts after `if (x != none)`).
  Deferred to a v2 (see §4, D3).
- Struct types (`plans/structs.md`). Inference is designed to *accommodate* them
  later (a `Struct<Name>` static type), but they're not built here.

## 2. The static type lattice

A static type `STy` is distinct from the runtime `Type *` (the ops table). New
representation, proposed header `src/stype.h` (+ `stype.cpp`, a normal TU added
to the Makefile glob list):

```
enum class STyKind {
    Unknown,    // a fresh type variable, no constraints yet (internal)
    None,       // the only-none / not-yet-pinned unit type (script-visible)
    Int,
    Float,
    Str,
    Array,      // + element STy
    Dict,       // + key STy + value STy
    Func,       // + param STys + return STy + per-param opt flags
    Exception,
    Struct,     // + StructTypeDef* (future)
    Dyn,        // explicit dynamic top; today's runtime behavior
};

struct STy {
    STyKind kind;
    bool opt;                 // nullable: "kind, or none"
    // payloads (only the relevant one is used):
    STyRef elem;              // Array element
    STyRef key, val;          // Dict
    std::vector<STyRef> params; std::vector<bool> param_opt; STyRef ret; // Func
    // Unknown carries a union-find link (see §3).
};
```

`STy` values are interned/shared (`STyRef` = pointer into an arena owned by the
inference context) so structural types compare by identity after
canonicalization and the graph stays small.

**Ordering of the lattice (the `<=` "assignable-to" relation).** `A <= B` means a
value of static type `A` may be stored where `B` is expected.

- `None <= T` for **every** `T` that is `opt` (none is assignable only to
  optionals) — and `None <= None`, `None <= Dyn`.
- `T <= T` (reflexive).
- `T <= opt T` (a non-null value fits an optional slot).
- `Int <= Float` (int promotes to float, matching `num_bin_op`). **One-way.**
  `Int <= opt Float`, etc., compose.
- `T <= Dyn` for all `T`; `Dyn <= T` is a *narrowing* — disallowed implicitly,
  requires the value to actually be dyn-typed at runtime (see D10).
- `Array A <= Array B` iff `A <= B` and `B <= A` (invariant — arrays are
  mutable; covariance would be unsound). Same for `Dict`. In practice both sides
  must be the *same* element type, or one is `Dyn`.
- `Func` is contravariant in params, covariant in return (standard); but since
  functions are monomorphic here (D2), we mostly need equality.

**Join (least upper bound), used to merge multiple assignments / branches /
return points / call-site arg types into one declared type:**

- `join(T, T) = T`.
- `join(None, T) = T` if `T` is already nullable, else `opt T`. `join(None,
  None) = None`.
- `join(Int, Float) = Float` (promotion). `join(Int, Int) = Int`.
- `join(T, opt T) = opt T`; nullability is sticky.
- `join(Int, Str)` and other incompatible concretes = **conflict**. A conflict
  is *not* silently widened to `Dyn` — it is a compile error (D2), unless an
  involved declaration is explicitly `dyn`.
- `join(Array A, Array B) = Array(join(A,B))` only when that keeps it a valid
  invariant array; mixed element types collapse the element to `Dyn` (D1).

Notes:
- There is **no separate Bool**: MyLang's `true`/`false` are `int` 1/0, `&&`/
  `||`/`!`/comparisons all yield `int`. So "boolean" == `Int` throughout.
- `Unknown` is the inference-time-only state of a type variable; by the end of
  solving every `Unknown` must resolve to a concrete type, `None`, or `Dyn`, or
  it's an error/defaulted (D2/D-finalize).

## 3. Representation in the codebase

- **`src/stype.h` / `stype.cpp`** — `STy`, `STyKind`, the arena/interner, and
  the lattice ops `assignable(a,b)`, `join(a,b)`, `unify(a,b)`, plus pretty-
  printing for error messages.
- **Type variables + union-find.** An `Unknown` STy is a union-find node with a
  representative pointer; `unify(a,b)` links them or, if both are resolved,
  checks compatibility (equality, with `Int<=Float` and `None` handled). This is
  the HM substitution, adapted with the nullability/promotion subtyping.
- **AST annotations.** Add a `STyRef static_type` field to `Construct` (every
  expression node gets its resolved type), analogous to the existing
  `Construct::sym` / `inline_ctx` additions. Decls/params/func-returns store
  their type via the symbol table (next bullet).
- **Symbol types.** Reuse the resolver's slot/symbol identity. Extend the
  per-symbol record (or a parallel `std::unordered_map<const UniqueId*, STyRef>`
  keyed like the resolver) so each var/const/param/func has one `STyRef`.
  `FuncDeclStmt` gains `STyRef param_types[]`, `STyRef return_type`, and
  `bool param_opt[]`.
- **New pass entry point:** `infer_types(Construct *root)` in a new
  `inferencer.cpp` (or folded into `resolver.cpp` next to `AutoConst`/`Inliner`;
  decision D9). Called from `mylang.cpp` after `resolve_names`.
- **New compile-time exceptions** (`errors.h`, `DECL_SIMPLE_EX` — *not*
  script-catchable, like `ExpressionIsNotConstEx`): `TypeInferenceEx` base, with
  `TypeMismatchEx` (a <= b fails / type change), `NullabilityEx` (none into
  non-opt, or deref of an opt), `ArityEx` (wrong arg count — distinct from the
  runtime `InvalidNumberOfArgsEx`), `AmbiguousTypeEx` (unresolved overload).
  Carry `Loc` for the caret + a message naming both types.

## 4. Key design decisions

These are the forks. **D1-D5 are now LOCKED** (confirmed with the user); D8/D9
follow the recommendation unless revisited.

**D1 — Container homogeneity. LOCKED: join, fall to Dyn when mixed.**
MyLang arrays/dicts are heterogeneous at runtime (`[1, "x", 3.0]` is legal). For
inference each container carries an element type = the **whole-program join of
everything inserted** (literal elements + every `append`/`push`/`insert`/`+=`/
subscript-store anywhere that array flows). If that join is a single concrete
type -> `Array<T>` (fast, typed `arr[i]`); if genuinely mixed ->
`Array<Dyn>` (stays heterogeneous, element access is dyn, no speed win, no
error). Same for `Dict<K,V>`. *Recommendation:* this "join, fall to Dyn when
mixed" rule — keeps existing heterogeneous arrays working while typing the
common homogeneous case. *Alternative:* make arrays strictly homogeneous and
error on mixed — simpler types, but a real language restriction. I recommend the
former.

**D2 — Type conflict. LOCKED: hard compile error.**
When a var/param/return is constrained to two incompatible concretes (`int` and
`str`) it is a `TypeMismatchEx` at compile time ("variable changes type"). `dyn`
is the deliberate opt-out — genuinely polymorphic functions/vars must say `dyn`.
No silent widening.

**D3 — Nullability model. LOCKED: locals inferred-nullable, narrowing deferred.**
Non-nullable by default; `opt` marks a nullable entity (Kotlin/Swift `T?`).
- **Params:** explicit — `func f(opt x)` accepts `none`; a plain param does not.
  Passing `none` (or an `opt`/`None` value) to a non-opt param -> `NullabilityEx`
  at compile time. This is the null-deref safety you want.
- **Locals:** *inferred* nullable — a var that is ever assigned `none` (incl.
  bare `var x;` before its first real assignment, and `var x = none;`) gets
  `opt T`. Using an `opt T` where a non-opt `T` is required (an arithmetic
  operand, a non-opt arg, a subscript base) -> `NullabilityEx`.
- **No flow narrowing in v1:** we do *not* yet recognize `if (x != none) {
  ...use x as non-opt... }`. So in v1 you either keep the value non-null, mark
  the consumer `opt`, or use `dyn`. v2 adds definite-assignment + smart-cast
  narrowing.

**D4 — Int/Float promotion as subtyping.** `Int <= Float`. An `int` argument
binds a `float` parameter (promoted), and a var assigned both int and float is
`Float`. Matches `num_bin_op`. Decided (not a fork) — it's how the runtime
already behaves.

**D5 — Surface syntax. LOCKED: `opt` + `dyn` modifiers only (no annotations).**
No general type annotations (pure inference). We *do* need keywords:
- `opt` — param modifier: `func f(opt x)`. Possibly also `var opt x;` to force a
  nullable local. Parsed like the existing `const` param modifier
  (`Identifier::const_param` -> add `opt_param`).
- `dyn` — opt-out modifier: `func f(dyn x)` (this param is dynamic), and
  `var dyn x;` / `const dyn x = ...` (this var is dynamic). A dyn entity behaves
  exactly as today (runtime dispatch, any type, type-changes allowed).
*Recommendation:* two new keywords `opt`, `dyn`, usable as var/param modifiers
only (no standalone type syntax). Lexer: add to `Keyword` enum + `KwString`.

**D6 — Builtins get hand-written static signatures.** Builtins are not
inferable (their bodies are C++). Provide a static signature table: `print(...)`
= variadic `Dyn -> None`; `len(Str|Array|Dict) -> Int`; `str(Dyn) -> Str`;
`int(...)`, `float(...)`, math builtins `(Float) -> Float` (with `Int<=Float`
inputs), `append(Array<T>, T) -> None`, `find(Dict<K,V>, K) -> opt V`, etc. A
table in `inferencer.cpp` mirroring the `builtins`/`const_builtins` maps. Some
builtins are genuinely polymorphic/variadic -> their relevant params are `Dyn`.
Decided (mechanism); the per-builtin signatures are filled in during M8.

**D7 — `==`/`!=` across types.** Equality is defined broadly at runtime
(int/float cross, str, array, dict). Type rule: `a == b` is well-typed if
`a`,`b` are *comparable* (same type after promotion, or one is `Dyn`/`None`);
result `Int`. Ordering `< <= > >=` is numeric-or-string only. Decided.

**D8 — Hard-fail vs transition flag. CONFIRM.**
The pass rejecting programs is a **behavioral change**: scripts that ran before
(relying on dynamic type changes) may now fail to compile. *Recommendation:*
ship behind a CLI flag during bring-up — `-nti` ("no type inference") disables
the pass (like `-nc`/`-ni`), default ON once the suite is green. Also: every
existing test/bench must pass the inferencer (or be marked `dyn`), which is part
of each milestone's done-criteria, not an afterthought.

**D9 — Pass ordering. CONFIRM.**
Current order: parse(+const-fold) -> `resolve_names` (slots, AutoConst,
Inliner). Where does `infer_types` go? *Recommendation:* **after `resolve_names`
returns** (so it has slots, auto-const results, and the inlined/specialized
tree), as its own pass. Const-folding/auto-const already evaluate constants
concretely, so they don't need the static types; running inference last means it
types the *final* runtime tree. (A later speed phase that specializes nodes
would run after inference.) *Risk to validate:* the inliner clones/rewrites the
tree — inference must run on the post-inliner tree, and any node it annotates
must already exist. Confirm we don't want inference *informing* inlining (we
don't, for v1).

**D10 — The `dyn` boundary.** Ops where any operand is `Dyn` produce `Dyn` and
defer to runtime dispatch (today's behavior). Assigning a `Dyn` into a typed
(non-dyn) slot is a **narrowing**: disallow implicitly. Provide it via runtime-
checked use only — i.e. you can't move `dyn` into `int x` without `dyn`-typing
`x` too (or a future `as`/cast). Decided; the cast operator is out of scope.

## 5. The algorithm (passes within `infer_types`)

Run as a sequence of sub-passes over the resolved AST:

**Pass A — scaffolding.** Walk every function (and top-level "main"). Allocate a
fresh `Unknown` STy variable for each var/const, each param, and each function's
return. Seed the obvious: `const`/`var` modifiers `dyn`/`opt`; builtins from the
signature table (D6); literal nodes get their ground type (`int`/`float`/`str`/
`None` for `none`).

**Pass B — constraint generation.** Walk the AST emitting constraints. Forms:

- Decl `var x = E;` / `const x = E;` -> `typeof(E) <= typeof(x)` and record
  `typeof(x)` ⊒ the join of all its initializer + later assignments.
- Assignment `x = E;` -> add `typeof(E)` to `x`'s assignment set (the var's type
  is the join over all assignments; `Int<=Float`, `None` handled). Compound
  `x += E` -> the operator rule below with `x` as both operands' carrier.
- Binary op (per the verified semantics):
  - `+` : (Int|Float, Int|Float)->numeric-join | (Str,Str)->Str |
    (Array<A>,Array<B>)->Array<join(A,B)>.
  - `- * / %` : (Int|Float, Int|Float)->numeric-join. Plus `Str * Int -> Str`
    (repeat; left operand str). `/` result is numeric-join (int/int->Int
    truncating; any float -> Float).
  - `< <= > >=` : (Int|Float, Int|Float) or (Str,Str) -> Int.
  - `== !=` : comparable(a,b) -> Int.
  - `&& ||` : (Int, Int) -> Int. (Verified: land/lor require int both sides,
    yield int; no short-circuit. So both operands constrained to `Int`.)
  - unary `- +` : (Int|Float)->same; `!` : (any truthy)->Int.
  Each emits a constraint that resolves once operand types are known (overload
  resolution, §3 worklist). Unknown operands defer.
- Call `f(a1..an)` -> `f` is `Func`; `arity == n`; `typeof(ai) <=
  param_type(f,i)` (with the opt/none check deferred to Pass E); call expr's
  type = `return_type(f)`. Higher-order: `f` may itself be a param/var of `Func`
  type; same constraints. Builtins use the signature table.
- `return E;` in function `g` -> `typeof(E)` added to `g`'s return join.
  A function with no `return` (or `return;`) contributes `None` to the return
  join. Single-expr body `=> E` -> return = `typeof(E)`.
- Subscript `c[i]` -> `c` is `Array<T>` (then `i:Int`, result `T`) or `Dict<K,V>`
  (then `i:K`, result `V`) or `Str` (`i:Int`->`Str`) or `Dyn`. Slice `c[a:b]` ->
  same container type, `a,b:Int`.
- Container literals: `[e1..]` -> `Array<join(typeof ei)>`; `{k:v,..}` ->
  `Dict<join k, join v>`.
- Control flow: `if/while/for` conditions impose no type constraint (everything
  is truthy in MyLang) beyond "not `None`-only used as a value"? — actually a
  bare `none` condition is degenerate; leave unconstrained. Loop bodies/branches
  contribute to the same var join (so `if (c) x=1; else x=2;` joins to `Int`).
- `throw E` -> `typeof(E)` is `Exception`. `catch (name)` binds `name :
  Exception` (or the specific dynamic exception type — keep `Exception` in v1).

**Pass C — solve to fixpoint.** Worklist over the constraints + union-find:
1. Unify equalities; propagate resolved types through `Unknown` reps.
2. When both operands of a deferred operator constraint are resolved, resolve
   the overload -> set the result type (or raise `TypeMismatchEx` if no overload
   matches and neither operand is `Dyn`).
3. Recompute joins for vars/params/returns from their accumulated sets.
4. Repeat until no type variable changes (fixpoint). Recursion and forward
   references converge here (start `Unknown`, tighten each round). A bound on
   iterations guards against non-termination bugs (should converge in passes ==
   call-graph depth-ish).

**Pass D — finalize/default.** For each still-`Unknown` variable:
- only-`none` (or no) constraints -> `None` (sound; "doesn't matter").
- conflicting concretes recorded -> `TypeMismatchEx` (unless `dyn`-declared).
- otherwise its single resolved concrete type.
Canonicalize all `STyRef`s to interned representatives; write final types into
the symbol table and `Construct::static_type`.

**Pass E — check.** A final walk asserting every constraint with final types:
- assignability at every assignment, arg-binding, return, container store;
- nullability: `none`/`opt`/`None` into a non-opt position -> `NullabilityEx`;
  use of an `opt` value as a non-opt operand (arith/subscript-base/non-opt arg)
  -> `NullabilityEx`;
- arity, operator validity, subscript-base validity.
Each violation throws the matching compile-time exception with a `Loc`. (Pass C
already raised the structural conflicts; Pass E is the assignability/nullability
gate that needs *final* types.)

## 6. Worked cases (must all behave as stated)

- `var x = 5; x = 7;` -> `x : Int`. `x = "a";` later -> `TypeMismatchEx`.
- `var x; x = 5;` -> `x : opt Int` (bare decl seeds none). Reading `x` before any
  assignment in an arith op -> `NullabilityEx` (it's opt). `x + 1` after a
  definite assign is still flagged in v1 (no narrowing) -> user marks intent or
  initializes `var x = 0;`. (This is the v1 nullability sharp edge; v2 narrows.)
- `var x = none;` never reassigned -> `x : None`. Fine.
- `func f(x) => x; f(3);` -> `x : Int`, `f : Func(Int)->Int`.
- `func f(x) => x; var myX = none; f(myX);` (no other call) -> `x : None`,
  `f : Func(None)->None`. Fine (the user's example).
- `func f(x) => x; f(3); var myX = none; f(myX);` -> `x : Int`; `f(myX)` passes
  `None` into non-opt `Int` -> `NullabilityEx`. Fix: `func f(opt x)` -> `x : opt
  Int`, both calls ok; `f`'s return `opt Int`.
- recursion: `func fib(n){ if(n<2) return n; return fib(n-1)+fib(n-2); }` ->
  `n:Int`, return `Int` (fixpoint).
- polymorphic misuse: `func id(x)=>x; id(3); id("a");` -> `x` joined Int & Str ->
  `TypeMismatchEx`. Fix: `func id(dyn x)=>x;` -> `x:Dyn`, return `Dyn`.
- containers: `var a=[1,2,3]; append(a,4);` -> `Array<Int>`;
  `append(a,"x");` somewhere -> element join Int|Str -> `Array<Dyn>` (D1),
  `a[i]+1` then becomes a `Dyn` op (no compile error, runtime dispatch).
- higher-order: `func apply(f,x)=>f(x); apply(g, 3)` with `g:Int->Str` ->
  `f:Func(Int)->Str`, `x:Int`, result `Str`. A second `apply` with a different
  function type -> conflict on `f` -> error (or `dyn f`).

## 7. Interaction with existing machinery

- **const-eval / auto-const / inliner** run before inference (D9); inference sees
  the final tree. Const literals already carry concrete values, so their types
  are trivially read off. The specialization clones (`$specN`) must be typed too
  (they're real functions in the tree by then).
- **`pure`/`effective_pure`** is orthogonal (purity != type). No change.
- **Backtrace / errors** unaffected at runtime; the new errors are compile-time
  (`DECL_SIMPLE_EX`), printed by `mylang.cpp` with a caret like other parse-time
  errors, *not* catchable by script `try/catch`.
- **`dyn`** values keep the entire current runtime path; inference simply does
  not constrain them. This guarantees a `-nti` / all-`dyn` program behaves
  exactly as today — the safety net during migration.

## 8. Testing strategy

New test category in `tests.cpp` (the existing table): two kinds —
- **must-type-check**: representative sound programs (all of §6's "fine" cases,
  the whole existing suite, every bench) run with inference on and don't throw.
- **must-reject**: each `&typeid(...)` set to the expected compile-time
  exception (`TypeMismatchEx`, `NullabilityEx`, `ArityEx`, `AmbiguousTypeEx`) —
  the §6 error cases. Plus caret-span checks (`ex_col`/`ex_line`) like the
  existing "err loc:" tests.
Also add `stype.cpp` unit-level checks (join/assignable/unify tables) as table
tests. The whole existing `-rt` suite + `bench/` must pass with inference ON
(part of each milestone's done-criteria), or the offending construct gets `dyn`.

## 9. Speed follow-on (separate phase, not this plan)

Once nodes carry final types, a later pass can **specialize** monomorphic nodes:
replace a generic `Expr03`(`*`/`/`/`%`) over two `Int` operands with an
`IntMulExpr` whose `do_eval` does the raw op — no `num_bin_op` promotion check,
no PMF dispatch, no in-`TypeInt` re-check. This captures the ~19%
promotion/dispatch block from the primes profile and the non-throwing-wrapper
elision, while **keeping the uniform `EvalValue do_eval()` interface** (the clean
~1.3-1.5x). Full *unboxing* (carrying raw `int_type` across node boundaries)
fractures that interface and is where a bytecode VM would win more — explicitly
out of scope. See the profiling discussion that motivated this whole track.

## 10. Milestones (each: build clean, `-rt` green, docs synced)

- **M0** — `stype.h`/`stype.cpp`: `STy`, lattice, `join`/`assignable`/`unify`,
  union-find, pretty-printer. Table unit tests. No AST wiring yet.
- **M1** — pass scaffolding: `infer_types(root)` skeleton wired into
  `mylang.cpp` behind `-nti`; `Construct::static_type` field; symbol-type map;
  Pass A literal/decl seeding. Runs, types nothing interesting, breaks nothing.
- **M2** — scalar core: constraint gen + solve + check for `int`/`float`/`str`/
  `none`, arithmetic, comparison, logical, var/const decl + assignment, `if`/
  `while`/`for` (no funcs, no containers). Gate scalar programs. `TypeMismatchEx`
  for type changes. Tests.
- **M3** — `opt`/`dyn` keywords (lexer + parser modifiers) + nullability checks
  for scalars (`NullabilityEx`). D3/D5.
- **M4** — functions: param/return inference, calls, recursion/mutual recursion,
  monomorphic join, arity, higher-order/`Func` types, captures. `ArityEx`,
  `AmbiguousTypeEx`. The §6 function cases.
- **M5** — containers: `Array`/`Dict` element/key/value inference via whole-
  program join, subscript/slice/literal/`append`-family typing, D1's fall-to-Dyn.
- **M6** — builtins signature table (D6) + `Exception`/`throw`/`catch` typing.
- **M7** — turn the pass ON by default; make the entire `-rt` suite + `bench/`
  pass (mark the genuinely-dynamic spots `dyn`); README (new `opt`/`dyn`
  keywords, the typing rules, the new compile errors) + CLAUDE.md (the new pass,
  `stype.*`, the new TU, `Construct::static_type`, errors) updated in the same
  commits.
- **M8 (separate)** — typed-node specialization for speed (§9).

## 11. Decisions (resolved)

1. **D2** — type conflict = **hard error**. LOCKED.
2. **D1** — heterogeneous arrays/dicts via **element-join-to-`Dyn`** when mixed.
   LOCKED.
3. **D3** — **locals inferred-nullable**; flow-narrowing (smart-casts) deferred
   to v2. LOCKED.
4. **D5** — two new keywords **`opt` + `dyn`** as the only new syntax. LOCKED.
5. **D8/D9** — ship behind `-nti`, on-by-default once green; run inference
   *after* `resolve_names`. Default per recommendation (revisit if needed).
6. Scope: deliver **inference + checking** first (M0-M7); **speed**
   specialization (M8) is a separate later effort.
