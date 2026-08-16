# The undefined / unbound name family

**THE AGREED DESIGN IS THE NEXT SECTION.** Everything after it is the
derivation that produced it, kept for the measurements; where the two
disagree, this section wins.

---

# AGREED DESIGN (maintainer, 2026-08-08)

## The mental model

Every declaration in a scope is **forward-declared at the top of that
scope** - the NAME exists from scope entry, the VALUE is bound when the
declaration statement runs. So

    func f() { return x; }
    var x = 3;

is understood as

    /* the name x exists here, unbound - NOT opt, just declared */
    func f() { return x; }
    x = 3;                    /* binds it */

The problem is therefore never "x is not declared". It is **"x is not
bound yet"**, which is a different, defined condition.

This is JavaScript's `let` **Temporal Dead Zone**, adopted deliberately
and applied at EVERY scope (maintainer changed position for consistency,
2026-08-08). The consequence, accepted:

    var loc = 99;
    func f() { var r = loc; var loc = 5; return r; }
    print(f());        # today: 99 (reads the OUTER loc)
                       # agreed: an ERROR - the inner `loc` is in its TDZ

To read the outer one, put the shadowing declaration in a nested scope:

    var loc = 99;
    func f() { var r = loc; { var loc = 5; ... } return r; }

## The five pieces

**FIX-0 - a brace-less body is its own scope. LANDED (6f0768f, #129).**

    if (c) var x = 5;
    print(x);          # was 5; now "Undefined variable 'x'"

**FIX-1 - a name declared NOWHERE is a COMPILE error. APPROVED (#130).**

    var scaled = 10;
    var total = scaled * heigth;      # typo - exists in no scope
    print(total);
                       # today: runs, then fails at run time
                       # agreed: refuses to compile

**TDZ - a read before the binding. Split by DECIDABILITY (#131).**

*Lexically decidable -> COMPILE error.* The reader is not inside a
function body, so its position relative to the declaration is fixed:

    func f() { var r = t; var t = 5; return r; }   # same scope
    var q = g; var g = 5;                          # top-level statement

**PROVEN decidable (measured 2026-08-08):** a nested function CANNOT
defer a local read - a lambda body cannot see an enclosing local at all
without an explicit capture list (`var t = 5; var g = func() => t;`
already fails with "Undefined variable 't'"; only `func[t]()` works). So
there is no way to make a local read happen later, and **locals need NO
runtime bound-flag and pay NO per-slot cost.**

*Not decidable -> RUNTIME `UnboundSymbolEx`.* A function body reading a
GLOBAL: the function may be called at any time.

    func fetch() { return g; }
    var dyn t = fetch();          # called BEFORE the binding
    var g = 5;

**`UnboundSymbolEx` is a `RuntimeException`** (catchable), which is the
whole point - see *What that buys* below.

**STATIC DETECTION - best effort, required for the easy cases (#131).**
Where a global read is provably unbound (a direct top-level call, before
the declaration, to a function whose body reads it unconditionally),
diagnose at compile time. Undecidable cases stay runtime, by design:

    func f() { return x; }
    var fns = [f];
    if (runtime(1)) { var dyn r = fns[0](); }   # before or after? runtime
    var x = 3;

**CARETS + FRAMES - in the batch (#126/#127/#128).** Required, not
polish: **HARD RULE 2** (CLAUDE.md) says an optimization may never change
observable behavior, and a caret that differs between the tree-walker and
the VM is a violation of it. Measured today, with `W` =
`var w = 0; for (var i = 0; i < 40; i++) { w += i; }` to defeat inlining:

    func fetch() { W return g + w; }     # READ
      tree-walker: ^^^^ over `g + w` (WRONG)   VM: ^ over `g` (right)
    func f() { W g[0] = w; }             # STORE
      tree-walker: ^ over `g` (right)   VM: ^^^^ over `g[0]` (WRONG)
    func mk() { W var c = func[g]() => g; return c; }   # CAPTURE
      tree-walker: the whole closure expr      VM: no caret at all

NEITHER engine is uniformly right. The capture case additionally needs a
`Loc` per `CaptureDesc` (today `{name, kind, slot}`) - serializable, so a
`.myv` format bump.

**(b) CONST-FOLD POISONING - #133. APPROVED.**

    func abs(x) { return 42; }
    print(abs(-1), abs(runtime(-1)));      # today: 1 42

When the parser parses a declaration named N and N is a const builtin,
**poison N in the current const scope**. `const_ctx` is already a lexical
scope chain pushed/popped by pBlock, so this covers `func`, `var`,
params, foreach vars and catch vars with no new structure. Refuse the
fold only when the visible binding is NOT the const evaluator's own - a
`pure func` IS that binding and must keep folding (`pure func p(x) => x*2;
const C = p(3);` must stay 6).

**REVISED 2026-08-08: `defined()` STAYS IN THE SCRIPT.** With the TDZ
model the two questions are genuinely different and both are useful:

| the name is... | `defined()` | `isbound()` |
|---|---|---|
| declared NOWHERE | `false` (folds; never fails) | compile error |
| declared, decl NOT yet run | **`true`** | `false` |
| declared, decl has run | `true` | `true` |

    func f() { var b = defined(a); var a = 5; return b; }
      today  -> false   (the resolver walks forward, so `a` is unresolved)
      at the end of the arc -> TRUE (under TDZ the name is declared for the
                                     whole scope; it is merely UNBOUND)

**This falls out of step 3 for free**, which is worth knowing before
touching it: `try_fold_defined` already folds to `true` for a
`SymKind::local`, so once TDZ hoists names, `defined(a)` before the decl
resolves to the local slot and folds `true` with NO change to that
function. What must change is the TEST - `tests.cpp` currently asserts
`defined(a) == 0` before `var a;` and will invert to `== 1`.

And the runtime half already exists under the wrong name: today's comment
in `try_fold_defined` says "a GLOBAL is a genuine runtime property (its
decl may not have run) -> left for DefinedGlobalV". That IS the bound
question, so **`DefinedGlobalV` simply changes owner from `defined()` to
`isbound()`**; `defined()` of a declared global folds to `true`.

**`isbound()` is the new builtin** - lazy (unevaluated argument, so
`mark_lazy_builtin` + the F1 rule: callable directly, never usable as a
value). The `.myv` builtin-set fingerprint changes.

**(superseded note) `defined()` -> REPL-ONLY.** After FIX-1 a
nonexistent name is a compile error, so the only false answer left in a
script is "declared but not bound yet" - which is what `isbound()` names
honestly. Both are lazy builtins (unevaluated argument), so `isbound`
needs `mark_lazy_builtin` and the F1 rule (callable directly, never used
as a value). The `.myv` builtin-set fingerprint changes.

## What that buys - the two unacceptable bugs die

Because `UnboundSymbolEx` is a `RuntimeException`:
- `vm_unwind_walk` is typed on `unique_ptr<RuntimeException>`, so the
  **backtrace frames come back** (#126);
- the JIT conveyance `g_vm_jit_exc` is typed the same, so the throw is
  carried instead of escaping a fragment as a raw C++ throw - **the
  SIGABRT dies** (#132's user-reachable door).

**And it costs nothing.** Measured: the definedness check is ALREADY
enforced in all three tiers, including inside JIT-compiled code -

    func f() { var s = 0; for (var i = 0; i < 2000; i++) { s += g; } return s; }
    var dyn t = f();
    var g = 5;          # -tw, -nj and vm+jit all report the error

so this is a rename plus a base-class change, not a new check.

**It also avoids FIX-2's worst cost.** The forward declaration is
conceptual (name only), so the real `var x = 3;` stays put and `x` keeps
its exact `int` type. The earlier FIX-2 shape forced `var x;` to the top,
which is infectious: `var x; x = 3; print(x + 1);` is a `NullabilityEx`,
and passing it to `func show(int n)` is an `OptRequiredEx`.

## Migration cost: ZERO, measured

- FIX-0: 0 brace-less declaring bodies in the corpus.
- FIX-1 / TDZ: the 8 shadowing candidates found earlier are ALL false
  positives - `st`/`n`/`n` are PARAMETERS (bound at entry, never a TDZ
  case), `a`/`b` in samples/gcd are inside a COMMENT, and 48_const_fold's
  inner `var dyn s = 0;` is declared before its uses.
- (b): 0 of the 77 const-builtin names is bound anywhere in samples/ +
  bench/my/ + tests/functional/.

## Settled details (maintainer, 2026-08-08)

**FUNCS AND STRUCTS HOIST WITH THEIR BINDING, AT EVERY SCOPE.** Global
scope and local scope must make NO difference for a function. This is
already true at top level and is what keeps mutual recursion and the
"helpers below main" layout legal:

    var dyn t = f();  print(t);   func f() { return 5; }      -> 5
    var p = P(1);  print(p.x);    struct P { int x; }         -> 1

It is BROKEN for a func/struct declared inside a function body (#134):

    func outer() { var dyn r = inner(); return r; func inner() { return 7; } }
      -tw -> Undefined variable 'inner'
      -nj/jit -> InternalErrorEx: construct not lowered natively

while the same shape inside an `if` block works. Target: all of these
work. This is the JavaScript split MyLang already half-implements -
`function` declarations hoist with their binding, `let`/`const` hoist
name-only with a TDZ - and only the `var` half needs new behavior.

**COMPILE-TIME DETECTION: PROVE -> FAIL, SUSPECT -> WARN, `--strict` ->
ENFORCE.** Three tiers:
 - a violation decided by a SPECIFIED rule (FIX-1's "declared nowhere",
   and lexical TDZ) is a hard **compile error** - its power is fixed by
   the spec, so it cannot drift;
 - a global read that a HEURISTIC proves will always be unbound is a
   hard **compile error** too (maintainer's call: "fail when we can prove
   that the failure is guaranteed");
 - anything merely suspicious is a **WARNING**, in GCC's spirit
   ("this variable might be uninitialized").

**A PROVABLE FAILURE IS A COMPILE ERROR EVEN INSIDE A `try`** (maintainer,
2026-08-08). The two exception kinds exist precisely so this is
expressible: `UseBeforeBindingEx` is a COMPILE error and is NOT
catchable, `UnboundSymbolEx` is the catchable runtime one. So

    func fetch() { return g; }
    try { var dyn t = fetch(); } catch (UnboundSymbolEx) { print("no"); }
    var g = 5;

is REFUSED at compile time - `fetch()` provably cannot work there.
Acknowledged as slightly inconsistent and chosen anyway, because it makes
code safer. **Catching `UnboundSymbolEx` should be an extremely rare
thing in real code, and the README must say so.**

The prover must be CONSERVATIVE: a false "provable" refuses a program
that would have run correctly. When in doubt it must fall back to the
warning tier.

**NEW `--strict` OPTION**: enforces FIX-2 in its ORIGINAL shape -
mandatory up-front declaration of every non-local before use - plus
whatever further strictness is added over time. Off by default; the
place to put rules that are too aggressive to impose on everyone.

**`isbound()` WORKS FOR LOCALS TOO** (maintainer corrected an error of
mine: I claimed it was trivially true, it is not). `isbound()` takes an
UNEVALUATED identifier, so asking about a name inside its TDZ is NOT a
read and NOT a violation - it is the whole point. The existing
`defined()` already demonstrates the exact semantics:

    func f() { var b = defined(x); var x = 5; return b; }  -> false
    func f() { var x = 5; var b = defined(x); return b; }  -> true
    func f() { return defined(g); }  var dyn t = f();
    print(t);  var g = 5;                                  -> false

So: for a LOCAL the answer is lexical (false before the declaration, true
after) and therefore **folds to a compile-time literal** - each loop
iteration re-runs the declaration, so "before the decl" is false on every
iteration and the lexical answer stays correct; no runtime bound-flag is
needed. For a GLOBAL read from a function body it is a genuine runtime
query on the existing `defined[]` flag. `isbound()` on a func/struct name
is always true.

`defined()` becomes REPL-ONLY. `isbound()` is its script replacement, a
lazy builtin (unevaluated argument -> `mark_lazy_builtin` + the F1 rule:
callable directly, never usable as a value). Messages name the symbol:
`Unbound symbol 'g'`, `Use before binding: 'g'`.

**PARKED IDEA, NOT NOW:** a C-`#ifdef`-style `isdefined()` that can ask
about a name declared NOWHERE without a compile error, so a script can
feature-test. It needs EARLY dead-code elimination, so the body under the
false branch never has to compile. Recorded as a separate future plan;
explicitly out of scope here.

## `UncatchableRuntimeException` - the fix for the abort (maintainer idea)

**The problem is NOT that a channel is missing - it is that the class
hierarchy conflates two unrelated things.** There are already TWO
conveyances out of a JIT fragment (vm.cpp):

    g_vm_jit_exc   std::unique_ptr<RuntimeException>
                   -> EnterNative re-raises through vm_raise
                   -> FRAMES + the baked caret
    g_vm_jit_eptr  std::exception_ptr - carries ANYTHING
                   -> EnterNative does a BARE rethrow -> NO frames

and **neither is automatic**. A helper called from native code must
CATCH its own exception and stash it in one of the two. A helper that
simply lets a C++ exception propagate out of itself passes through
JIT-generated code, which carries **no unwind information**, so the
unwinder finds no handler -> `std::terminate`.

That is the whole SIGABRT: the closure path (`MakeClosureV` ->
`FuncObject` ctor -> `read_sym` -> `RValue` -> throw) has NO
catch-and-convey, because it was written on the assumption that it could
not throw.

So there are THREE separable problems, and today's design forces them to
be decided together:

 1. **missing conveyance at a site** -> the abort. Independent of class.
 2. **the eptr channel loses frames** -> a non-RuntimeException that IS
    conveyed still renders without a backtrace.
 3. **catchability** -> a LANGUAGE question.

`RuntimeException` currently means BOTH "travels the good channel with
frames" (implementation) and "a script `catch` may name it" (language).
**Splitting them is the fix:**

    Exception
     |- RuntimeException                   travels g_vm_jit_exc: frames + caret
     |   |- UncatchableRuntimeException    a `catch` clause naming it is a
     |   |   |                             COMPILE error
     |   |   |- InternalErrorEx
     |   |   \- UndefinedVariableEx       (REPL runtime; script = compile err)
     |   \- the catchable ones: DivisionByZeroEx, OutOfBoundsEx, ...,
     |      and the new UnboundSymbolEx
     \- the pure COMPILE-time ones: SyntaxErrorEx, TypeMismatchEx,
        UseBeforeBindingEx, NotLoweredEx, ...

Enforcement is static and cheap: catch-clause type names are already
known at compile time (they are interned into the `catch_types` pool), so
naming an uncatchable one is rejected there. This gives every RUNTIME
error - catchable or not - frames, a caret and a real message, with no
`std::terminate` anywhere, while keeping `UndefinedVariableEx`
uncatchable in a script as required. Longer term `g_vm_jit_eptr` can be
DELETED: once every runtime-raisable exception is a `RuntimeException`,
it has nothing left to carry.

### When does `InternalErrorEx` actually fire? (census, 2026-08-08)

Two different things share the name:

**(a) `NotLoweredEx`** (`struct NotLoweredEx : public Exception`,
codegen.cpp) - a **COMPILE-time** refusal, printed as "InternalErrorEx:
codegen: construct not lowered natively". It is USER-REACHABLE today, and
every instance is a bug - the no-fail codegen refusing a LEGAL program:

    zz[0] = 1;                                          -> #130 (FIX-1)
    func outer() { var dyn r = inner(); return r;
                   func inner() { return 7; } }         -> #134

After FIX-1 and #134 **no valid user program should reach it.**

**(b) runtime `InternalErrorEx`** - "proven impossible" tripwires:
`get_vec()`/`get_view()` on a flat array (sharedarray.h), `pod_get`/
`pod_set` field validity (structtype.h), the type-erasure ops (type.h),
`default:` arms of switches over closed enums, and the codegen-proved
arms in vm.cpp (`/* base proven an array */`, `/* unreachable:
base_array general proven */`). **These fire only when the INTERPRETER
has a bug** - never on user input.

So "we don't want InternalErrorEx ever" resolves into: (a) is a real bug
class, fix it and it goes away; (b) must STAY - it is the tripwire that
found (a) - but must RENDER instead of aborting, which is exactly what
`UncatchableRuntimeException` provides.

## THE BATCH - 7 steps, in order (approved 2026-08-08)

Each lands as its OWN commit.

1. **FIX-1** (#130) - a name declared NOWHERE is a compile error
   (`UndefinedVariableEx`, raised at compile time). Includes migrating
   ~50 `defined()` test sites and 32 `UndefinedVariableEx` test
   expectations. **NOTE the implementation constraint:** the check needs
   a per-scope set of ALL declared names (including ones declared LATER),
   because `isbound(x)` before `var x` must stay legal while
   `isbound(zz)` must be refused - so "unresolved AT THIS POINT" is the
   wrong test; "declared nowhere in any enclosing scope" is the right
   one. This does NOT change resolution (that is step 3).
2. **`UncatchableRuntimeException`** - split "travels the good channel"
   from "a script may catch it". Small, independent, and it must come
   BEFORE steps 3-5 so their tests assert the final rendering rather than
   an intermediate one.
3. **TDZ + `UnboundSymbolEx` + `UseBeforeBindingEx`** (#131) - **LANDED.**
   Split by decidability, WITHOUT hoisting slots: the check consults
   `Scope::var_names` (the var/const subset of the FIX-1 pre-scan) and
   refuses the program in exactly the cases where hoisting would have
   changed the answer, so resolution itself is untouched. `defined()`
   folds TRUE for a TDZ name via `Identifier::in_tdz`, and for a global.
   Two bugs fell out: `jit_make_closure` was a `void noexcept` helper
   documented as "never throws" (that, not missing unwind info, was the
   SIGABRT), and capture descriptors were snapshotted BEFORE pass 2
   stamped escaped globals, so a captured global silently fell back to
   the by-name map walk.
4. **Carets** (#126/#127/#128) - incl. a `Loc` per `CaptureDesc`, so a
   `.myv` format bump.
5. **Nested func/struct hoist with binding** (#134).
6. **Const-fold poisoning + the `isbound()` builtin** (#133); `defined()`
   becomes REPL-only.
7. **Prover conservatism**, then **`--strict`**.

## Known residuals (accepted)

1. **`InternalErrorEx` inside a JIT fragment still SIGABRTs.** It is a
   plain `Exception` too. Only interpreter BUGS raise it, so it is not
   user-facing - but it makes our own bugs undebuggable. Low-priority
   hardening; #132 shrinks to this.
2. **Static detection is provably incomplete** (by design), so
   `UnboundSymbolEx` is permanent - which is exactly why the caret and
   frame work is in the batch rather than deferred.
3. **The REPL keeps `UndefinedVariableEx`** for a name no input has
   declared. Correct: the REPL is open-world, and it already renders
   message + caret + frames properly.

## Why this instead of fixing the backtraces

#126 (the VM renders no backtrace for an uncatchable error) and #127 (a
wrong caret on an undefined store base) were about making two engines
AGREE on how to report `UndefinedVariableEx`. Working through them
produced a better answer: **make the error unreachable in a compiled
script.** Three changes do that, and two of them are bug fixes rather
than language changes.

The evidence that this is the right shape:

- A script's runtime symbol map is EMPTY and asserted, so an unresolved
  name can ONLY fail at runtime - turning it into a compile error costs
  nothing (layer 1).
- `const` already REQUIRES declare-before-use: `const A = B + 1; const
  B = 10;` is `ExpressionIsNotConstEx` today, because const-eval runs in
  the parser top-to-bottom. Layer 2 makes `var` obey the rule `const`
  already obeys - a consistency fix, not a new concept.
- The remaining way to reach an undefined name - a declaration the
  control flow skipped - only exists because of a scoping BUG (layer 0).

After all three, `UndefinedVariableEx` cannot fire in a script. The REPL
keeps it, by design (open world) - and the REPL already renders it
correctly, verified:

    >> func run() { var dyn r = fn(); return r; }
    >> var dyn t = run();
    Undefined variable 'fn' at line 1, col 26:28
    Backtrace (most recent call first):
      [0] run()  at line 1
      [1] main() at line 2

because REPL top-level names stay map-resident and never take the
global-slot path that loses frames.

---

## Layer 0 - a brace-less body is its own scope (BUG FIX) - **LANDED**

**Status: DONE 2026-08-08.** `pStmtDeclaresName` / `pWrapDeclBody` /
`pBraceLessBody` (parser.cpp). It needed one mechanism the plan below did
NOT anticipate: the Block wrapper alone does not scope a `const`, because
a const decl registers its VALUE in the parser's const context AS IT IS
PARSED - so `pBraceLessBody` pushes the const + CSE scopes AROUND the
parse, exactly as pBlock does, and only then wraps. And the wrap was
extended to the CONST-FOLDED taken branch, closing a third bug of the
same family: const-eval was CHANGING SCOPING (`if (1) func g() {...}
g();` worked by default and threw under `-nc`).

Verified: all six leak forms now error; the three engines agree; `-nc`
and `--no-opt all` agree with the default. `-rt` 1761/1761 in five modes
across dbg / rel-hard / clang / CMake; corpus_diff 14/14 plain and
`--cold`; samples unchanged; `.myv` round trip byte-identical on five.
Both mechanisms are pinned by watched-failing sabotage (removing the
`Expr14` case fails 5 tests; removing the const-scope push fails 1).

### What is wrong

A declaration in a brace-less `if`/`else`/`while` body LEAKS into the
enclosing scope. Measured:

| form | today |
|---|---|
| `if (c) var x = 5; print(x);` c TRUE | prints `5` |
| `if (c) var x = 5; print(x);` c FALSE | prints `<none>` |
| `if (c) { var x = 5; } print(x);` | error (correct) |
| `if (c) const K = 7; print(K);` | prints `7` |
| `if (c) var a = 1; else var b = 2; print(a, b);` | prints `1 <none>` |
| `for (...) var w = 7; print(w);` | error (correct) |
| `if (c) func g() {...} g();` | error (correct) |

The if/else line is the one that settles it: `b` is visible **from the
branch that never ran**. And the language is inconsistent four ways -
braced scopes but brace-less does not; brace-less `func`/`struct` scopes
but brace-less `var`/`const` does not; `for` scopes but `if` does not.

### It is not a scoping wart - it BREAKS A TYPE PROOF (measured)

The leak lets a declaration be SKIPPED while its name stays in scope and
its inferred type stays `int`. The slot then holds `none` while every
consumer was compiled on the proof that it holds an int - and the three
engines disagree about what happens next:

    func f() { if (runtime(false)) var x = 5; return x + 1; }
    print(f());

| engine | result |
|---|---|
| VM + JIT (the DEFAULT) | prints `1` - reads `none` as int 0, SILENTLY |
| VM, `-nj` | ML_VM_CHECK aborts: `lv.is<int_type>()` failed |
| tree-walker | `TypeErrorEx` (catchable) |

A silent wrong number in the shipping engine is the worst outcome in
this project's error taxonomy, and it is NOT a JIT bug: the JIT elides
the tag check BECAUSE the inferencer proved the type (the
`static-types-imply-runtime-guards` rule). That proof is sound only if a
declaration cannot be skipped - which is exactly what layer 0
establishes. The float twin prints `1.000000`; a `str` local throws in
both engines (it has no unboxed tier to mis-read). The `else`-branch
form (`if (runtime(true)) var a = 1; else var b = 2; return b + 1;`)
prints `1` the same way.

LATENT, not active: the corpus has zero brace-less decl bodies (below).
But it makes layer 0 a CORRECTNESS fix rather than a tidying one, and it
is why layer 0 goes first.

### Why it is like this

`pWrapDeclBody` (parser.cpp) wraps a brace-less body in a synthetic
single-statement Block **only when the body is a func/struct decl** - the
targeted fix for the 2026-07-14 crash. The blanket wrap was deliberately
declined then, to preserve the historical `var`/`const` leak. That was a
decision to keep existing behaviour, not a finding that it is correct;
the maintainer has now revisited it.

### The change

`pWrapDeclBody` wraps EVERY brace-less body, not only decl bodies.

### What it touches

- **`src/tests.cpp:4301`**, `"brace-less var/const bodies keep the
  enclosing-scope binding"` - asserts the leak. It INVERTS: the same
  program must now fail. Keep the test, flip its expectation, and keep
  the func/struct sibling test at 4293 as-is (it already asserts
  scoping).
- **Corpus: ZERO hits.** A scan of `samples/` + `bench/my/` +
  `tests/functional/` for a brace-less body whose statement is a
  `var`/`const` decl found none.
- **Watch the const-fold quirk**: `if (1) func g() => 41;` const-folds
  to the bare decl, so `g` stays in the enclosing scope. That is the
  feature-flag pattern and the 4301 test pins it. Confirm the wrap does
  not disturb the const-true fold path (the `if` applies the wrap only
  to the RUNTIME statement - see the existing comment in `pStmt`).

### Why it matters beyond tidiness

With brace-less bodies scoped, **a declaration can never be skipped
while its name is still in scope**: a local's scope begins at its
declaration, `break`/`continue`/`return` leave the scope entirely, and a
loop re-runs its declarations per iteration. That removes the need for
any definite-assignment analysis, which an earlier version of this plan
had as a fourth layer.

---

## Layer 1 - an unresolved name in a script is a COMPILE ERROR (free)

### What is wrong

A name declared nowhere is a RUNTIME error today:

    var dyn total = scaled * heigth;      # typo
    -> Undefined variable 'heigth' at run time

### Why the fix is free

In a SCRIPT every name is slotted (local / global / capture / builtin) -
CLAUDE.md's "the script runtime symbols map is EMPTY (asserted)". So an
unresolved name reaches `lookup` on an empty map and is GUARANTEED to
fail. Making it a compile error converts a certain runtime failure into
a build failure; it removes no capability.

The codegen already agrees in spirit: an unresolved name in an rvalue
position lowers to a `ThrowRuntimeV(undefined_var)` that always throws
(codegen.cpp:1066).

### The change

The resolver (or the inferencer's strict block, alongside the
mandatory-`dyn` / mandatory-`opt` passes) rejects a name that resolves
to `SymKind::unresolved` in a script. Not in the REPL, which is
open-world - the same gate `strict_dyn` already uses.

### Open point: `defined()`

`defined(x)` on a name that exists nowhere is statically FALSE, so it
should FOLD to `false` rather than be rejected. Check the three
identifier forms (`try_fold_defined`, `DefinedGlobalV`, and the
non-identifier expression form) so the fold happens before the new
rejection sees the name.

---

## Layer 2 - declare-before-use for non-local variables (the language change)

### The rule

A reference to a top-level (non-local) VARIABLE must be lexically below
its declaration. Violating it is a compile error.

**Functions and structs still HOIST** - mutual recursion must keep
working, and a function is a compile-time entity with no runtime
initialisation order. So a body may call a function declared below it
but may not read a variable declared below it. That is exactly C++'s
file-scope rule.

`const` already obeys this (see the top of this document), so the change
is `var` catching up.

### Migration - measured

A heuristic scan of `samples/` + `bench/my/` + `tests/functional/` found
**8 candidate forward references, and all 8 are FALSE POSITIVES** - a
function PARAMETER or an inner local shadowing a top-level variable of
the same name (`n` in 43_sieve and 46_matrix_mult, `a`/`b` in
46_matrix_mult and samples/gcd, `st` in 76_funcval_dispatch, `s` in
48_const_fold, which is a `var dyn s` local inside `heavy`).

So the expected migration cost is ZERO - but a textual scan cannot prove
it. **Land the check as a WARNING first**, run the whole corpus + `-rt` +
the samples, collect the real list, fix whatever it names, and only then
flip it to an error. That is the step that discharges "fix all the
scripts that depend on this behaviour".

### The cost to users, measured

A genuine forward reference must become a forward DECLARATION, and that
is not free:

    var x;
    x = 3;
    print(x + 1);
    -> NullabilityEx: possibly-none value used in an arithmetic
       operation (type 'int?')

and it is infectious:

    func show(int n) { return n; }
    print(show(x));
    -> OptRequiredEx: parameter 'n' may be none; declare it 'opt'

So `var x;` does not merely make `x` nullable - it forces `opt` (or a
narrowing check) through every signature the value reaches. Worth
stating in the README when this lands: prefer declaring at the point of
first assignment.

### What layer 2 does NOT cover: LOCALS (measured 2026-08-08)

A local use-before-decl splits into exactly three shapes, and NONE of
them needs layer 2. **No nested function or closure is required to build
any of them** - each is a flat body.

**(A) no outer binding of that name** - already an error, and layer 1 is
what generalises it:

    func f() { var r = loc; var loc = 5; return r; }
    -> Undefined variable 'loc'

(today at run time; layer 1 moves it to compile time.)

**(B) an outer binding exists** - resolves to the OUTER one:

    var loc = 99;
    func f() { var r = loc; var loc = 5; return r; }
    print(f());          -> 99

This is C++'s own rule (a declaration's scope begins at its declarator),
so C++ does not diagnose it either. It is the ONLY shape a "layer 2 for
locals" would change, and changing it would make MyLang STRICTER THAN
C++ for no measured benefit. Same result for two locals in nested blocks
inside one function (verified: also 99).

**(C) the declaration is SKIPPED** - the layer-0 bug, and it is a LOCAL
bug exactly as much as a global one: every divergence example in the
layer-0 section above is a function local.

So the ordering rule stays a NON-LOCAL rule.

---

## Order, and what each step can be verified against

0 -> 1 -> 2. Each is independently landable, and each has its own net.

| step | net |
|---|---|
| 0 | flip tests.cpp:4301; add if/else + while cases; corpus_diff (0 expected changes) |
| 1 | new compile-error tests; confirm `defined()` still folds; REPL untouched (a `repl:` test) |
| 2 | the WARNING run over corpus + samples + `-rt`, then the flip; new compile-error tests; a `repl:` test proving the REPL is exempt |

Every step needs the standard battery: `-rt` in all five modes, rel-hard,
clang, `corpus_diff.sh` incl. `--cold` and `--levers`, the fuzzer, the
non-JIT platform probe, CMake, and a `.myv` round trip.

## Documentation that must change in the same commits

- **README.md** - the declaration rules (layer 2 is script-visible), the
  brace-less-body scoping (layer 0), and the new compile errors.
- **CLAUDE.md** - the `pWrapDeclBody` paragraph under *Invariants &
  hazards* explicitly documents the leak as deliberate and lists the
  behaviours it preserves; that paragraph becomes wrong at layer 0. The
  *Slot resolution* section's "a truly-undefined name -> the runtime map
  -> UndefinedVariableEx" becomes wrong at layer 1.

## What this deletes when it lands

- **#126** (no VM backtrace for an uncatchable error), **#127** (the
  store-base caret), **#128** (the read-family disagreement) - all
  unreachable in a script. Close them as obsoleted, not fixed.
- The `defined[]` runtime flag becomes provably always-true in a script
  once the decl has run, which makes G1 increment 6's argument
  (`5e1d9eb`, the removed `defined` probe at the emitted call site) a
  proof rather than an invariant maintained by hand.
- The `UndefinedId` sentinel's RUNTIME role shrinks to the REPL; its
  parser/REPL declaration-vs-assignment role stays.

## Adjacent finding - the same channel gap can ABORT the process (#132)

An undefined name in a CAPTURE LIST does not merely lose its backtrace
on the VM path - with the JIT on it kills the process:

    var c = func[zz]() => zz; print(c());

| engine | result | exit |
|---|---|---|
| VM + JIT (DEFAULT) | `terminate called after throwing ... UndefinedVariableEx` | 134 (SIGABRT) |
| VM, `-nj` | `Undefined variable 'zz'` (no caret, no frames) | 1 |
| tree-walker | full message + caret + frames | 1 |

Same family as #126: `UndefinedVariableEx` is a plain `Exception`, and
the JIT's conveyance (`g_vm_jit_exc`) is typed on `RuntimeException`, so
it is not carried and propagates as a raw C++ throw out of the fragment.
The exact mechanism (whether it is the missing conveyance alone or also
unwinding through JIT-emitted code) is NOT yet confirmed - that is the
first step of #132.

Layers 0-2 make this SHAPE unreachable in a script, but the CHANNEL gap
survives for any other plain `Exception` raised from inside a native
fragment. Worth fixing independently of this plan.

---

# DECISIONS + MEASUREMENTS, 2026-08-08 (after FIX-0 landed)

Everything below re-measured at HEAD. The layers are renamed FIX-0/1/2
here because "layer N" was not memorable; same three changes.

## Maintainer decisions recorded

1. **FIX-1 is APPROVED** ("a no-brainer: the variable is declared
   nowhere, there is no chance it could possibly work"). Task #130.
2. **`InternalErrorEx` and `SIGABRT` are unacceptable bugs** and must go.
   See the reachability answer below for which fix removes which.
3. **#133 proposal on the table**: disallow a script from overriding the
   CONST builtins, keep allowing the non-const ones. Completeness
   analysed below - it is NOT complete as stated.

## Which fix removes which unacceptable symptom

    zz[0] = 1;                       -> VM: InternalErrorEx (refuses to
                                        COMPILE a legal program)

**FIX-1 alone.** Measured mechanism: when the name is declared NOWHERE it
has no slot, so codegen has no lvalue to lower. The forward-declared twin
DOES lower -

    func f() { g[0] = 1; }
    var dyn t = f();
    var g = [1, 2];                  -> VM: "Undefined variable 'g'"
                                        (wrong caret, no frames - but no
                                         InternalErrorEx)

so `InternalErrorEx` is EXCLUSIVELY a declared-nowhere symptom and FIX-1
kills every instance.

    var c = func[zz]() => zz;
    print(c());                      -> VM+JIT: SIGABRT, exit 134

**Reachable from BOTH origins, so it needs BOTH fixes:**
- `func[zz]` with `zz` declared nowhere -> FIX-1;
- `func[g]` with `var g` declared BELOW -> FIX-2.

**The REPL could not be made to abort** in any shape tried (it renders
message + caret + frames). That is probably because the closure creation
did not land inside a JIT fragment, NOT a design guarantee - do not claim
the REPL is immune.

## The residual channel gap (#132) is SMALLER than feared

The abort happens because `UndefinedVariableEx` is a plain `Exception`
and the JIT conveyance is typed on `RuntimeException`. Blast radius
measured: the plain-Exception family is `InternalErrorEx`,
`CannotRebindConstEx`, `CannotRebindBuiltinEx`, `ExpressionIsNotConstEx`,
`AlreadyDefinedEx`, `UndefinedVariableEx`. Of these, at RUNTIME from a
VM op only `InternalErrorEx` (an interpreter-bug tripwire) and
`UndefinedVariableEx` can fire - the rebind ones are caught by the
resolver at COMPILE time (verified: a `const` param write and an
`abs = 9` assignment both fail before the program runs).

**So after FIX-1 + FIX-2 the channel gap has no user-reachable trigger
left in a script.** Repairing the channel becomes optional hardening
(it would still turn an interpreter-bug `InternalErrorEx` inside a
fragment from SIGABRT into a message).

## #133 - the same root cause as FIX-0, and the proposed rule is INCOMPLETE

    func abs(x) { return 42; }
    print(abs(-1), abs(runtime(-1)));        -> prints:  1 42

**Root cause, and it is the FIX-0 cause again:** the parse-time const
evaluator runs BEFORE name resolution, so it has its own - wrong - notion
of what a name means. FIX-0 was that disagreement about SCOPE
(`if (1) func g(){...}` leaked while the runtime form did not); #133 is
the same disagreement about SHADOWING. Both are "an optimization that
changes semantics".

**The proposed rule (ban overriding const builtins) does NOT close it,**
because the hole is not about *overriding* - it is about any BINDING of
that name. Measured:

    func f(abs) { return abs(-1); }          # a PARAMETER, not a redefinition
    var dyn g = func(x) => 42;
    print(f(g));                             -> 1     (the builtin ran)

    func f(abs) { var dyn n = runtime(-1); return abs(n); }
    print(f(g));                             -> 42    (the parameter ran)

    var dyn g = func(x) => 42;
    foreach (abs in [g]) { print(abs(-1)); } -> 1     (the builtin ran)

    struct E { int x; }
    try { throw E(1); } catch (E as abs) { print(abs.x); }
                                             -> TypeErrorEx: Expected dict
                                                object   (`abs` read as the
                                                builtin)

Which declaration forms ARE refused today (`CannotRebindBuiltinEx`, all
at parse time): top-level `var`, `const`, a function-local `var`, and a
lambda-bound `var`. `struct abs` gives `AlreadyDefinedEx`. **NOT refused:
`func abs`, a PARAMETER named `abs`, a `foreach` variable, a `catch`
variable.**

So the two coherent options are:

  (a) **extend the ban to EVERY binding form** - a const-builtin name may
      never be bound to anything, anywhere (var/const/func/struct/param/
      foreach var/catch var). Enforceable in one place, the way `_` is
      reserved in the inferencer's `new_sym`. COST: bans `func f(max)`,
      `foreach (min in ...)`, `catch (E as str)` - and there are many
      const builtins, so this bites ordinary code;

  (b) **fix the FOLD instead** - never const-fold a call whose callee name
      is bound by a declaration visible at that point, so a shadowed name
      is simply left for runtime. Preserves `func f(abs)`. This is the
      same shape as FIX-0's fix (make the const evaluator agree with the
      real name environment) and is the recommended direction, but it
      needs a way for the parser to know what is declared - which is
      exactly what it does not have today.

### What (b) COSTS - measured 2026-08-08, and the answer is "nothing"

The worry was that refusing the parse-time fold would lose an
optimization whenever a user function is auto-pure. It does not, because
**the LATER fold layer already resolves shadowing correctly** - AutoConst
runs AFTER resolve_names, so the callee is already bound to the user's
global slot. Measured with an argument that is const only AFTER
auto-const promotion, so the PARSER cannot fold it and AutoConst must:

    func abs(x) { return 42; }
    var k = -1;                  # write-once -> auto-const promoted
    print(abs(k));               -> 42, and the optimized tree shows
                                    DirectBuiltinCallExpr(print,
                                      ExprList(Int(42)))

So the call still folds to a literal - to the RIGHT literal. (b) does not
delete the fold, it moves it one layer down to where the name is known.

The four cases, exhaustively:

 1. NO shadowing (the normal case) - the fold looks for a binding, finds
    none, proceeds exactly as today. Byte-identical trees. **Corpus:
    ZERO of the 77 const-builtin names is bound anywhere in samples/ +
    bench/my/ + tests/functional/, so (b) changes no program we have.**
 2. Shadowing + an auto-pure user func - parse-time fold refused,
    AutoConst folds it correctly (measured above). No loss.
 3. A `pure func` inside a `const` decl - this is the ONE thing (b) can
    break, and it is a specification detail, not a cost:

        pure func p(x) => x * 2;
        const C = p(3);
        print(C);                -> 6 today, and MUST stay 6

    A naive "refuse if any binding exists" would refuse this and
    `const C = <non-const>` is ExpressionIsNotConstEx. So the rule is:
    **refuse only when the visible binding is NOT the one the const
    evaluator is about to use.** A pure func IS that binding.
 4. A plain (non-pure) user func inside a `const` decl - ONE new error,
    and it is an improvement:

        func f(x) { return 42; }    const C = f(3);
          -> ExpressionIsNotConstEx today (correct)
        func abs(x) { return 42; }  const C = abs(-1);
          -> 1 today (silently the BUILTIN); ExpressionIsNotConstEx
             under (b), consistent with the line above

**The cost of (b) is therefore compile-time ENGINEERING, not runtime.**
The parser must know what is declared, and today it tracks const scopes
only. Cheapest known mechanism: when the parser parses a declaration
named N and N is a const builtin, **POISON N in the current const
scope** (remove/mask the builtin entry). `const_ctx` is already a proper
lexical scope chain pushed and popped by pBlock, so the poison is
naturally scoped and covers params, foreach vars and catch vars as well
as `func`/`var`. No new data structure, no pre-pass.

Versus (a): (a) costs zero to implement and costs users 77 reserved
words forever; (b) costs users nothing and costs one parser mechanism.

**DECIDED 2026-08-08: (b), via poisoning.** Maintainer approved.

## What persists with FIX-1 + (b) but WITHOUT FIX-2

Measured at HEAD with inlining defeated (a small callee is spliced, which
removes the physical frame and would fake a "0 frames" result). `W` below
is `var w = 0; for (var i = 0; i < 40; i++) { w += i; }`.

**P1 - a READ of a global whose decl has not run yet.**

    func fetch() { W return g + w; }
    var dyn t = fetch();
    var g = 5;
    print(t);

| engine | caret | frames |
|---|---|---|
| tree-walker | `^^^^` over `g + w` - **too wide** | 2 |
| VM `-nj` | `^` over `g` - **correct** | **0** |
| VM + JIT | `^` over `g` - correct | **0** |

**P2 - a STORE whose base global has not run yet.**

    func f() { W g[0] = w; }
    var dyn t = f();
    var g = [1, 2];

| engine | caret | frames |
|---|---|---|
| tree-walker | `^` over `g` - **correct** | 2 |
| VM `-nj` | `^^^^` over `g[0]` - **too wide** | **0** |
| VM + JIT | `^^^^` over `g[0]` - too wide | **2** |

**P3 - a CAPTURE of a global whose decl has not run yet.**

    func mk() { W var c = func[g]() => g; return c; }
    var dyn h = mk();
    var g = 5;
    print(h());

| engine | caret | frames | exit |
|---|---|---|---|
| tree-walker | whole `func[g]() => g` | 2 | 1 |
| VM `-nj` | **none** | 0 | 1 |
| VM + JIT | none | 0 | **134 (SIGABRT)** |

Three conclusions the earlier framing got wrong:

1. **Neither engine is uniformly right about the caret.** The
   tree-walker is wrong on the READ (it carets the whole `g + w`); the VM
   is wrong on the STORE (it carets `g[0]`). "#127 = the VM's caret bug"
   understates it - #128's "the tree-walker disagrees with itself" is the
   accurate half.
2. **The missing backtrace is not a clean "the VM has none".** P2 has
   frames WITH the JIT and none without it. So #126 is
   shape-and-tier-dependent, not a single missing call.
3. **The SIGABRT survives FIX-1.** P3 is its second door and only FIX-2
   (or repairing the channel) closes it.

## What FIX-2 would COST - measured, and it is not small

FIX-2 makes a lexically-forward reference a compile error, and that is
NOT the same as "the reference was broken". Both of these WORK today and
would STOP compiling:

    func fetch() { return g; }
    var g = 5;
    var dyn t = fetch();          -> 5 today
    print(t);

    func total(a) { return a + base; }
    func run() { return total(1); }
    var base = 100;
    print(run());                 -> 101 today

This is the ordinary "helpers at the top, configuration at the bottom"
layout. FIX-2 would force every global a function reads to be declared
above the first function that mentions it. That is a real, recurring
ergonomic cost paid by CORRECT programs, to remove errors that only
appear in INCORRECT ones - and the earlier migration scan (0 real hits)
measured only whether the CORPUS trips it, not how often the pattern is
natural to write.

**So the FIX-2 decision is genuinely open, and there is an alternative
that removes P1/P2/P3 without any language restriction: repair the
REPORTING (#126 frames, #127/#128 carets, #132 channel).** FIX-2 makes
the errors unreachable; repairing reporting makes them correct. Both end
the divergence; only FIX-2 costs users something.

## Open questions for the maintainer

1. ~~Does layer 2 extend to LOCALS?~~ **ANSWERED 2026-08-08, measured** -
   see *What layer 2 does NOT cover: LOCALS*. No: shape (A) is layer 1,
   shape (B) is C++'s own behaviour, shape (C) is layer 0. No closure or
   nested function is needed to reproduce any of them.
2. Is `defined()` still meaningful in a script after layers 0-2? It can
   only be false for a global whose decl has not run - which layer 2
   makes impossible. It may become a REPL-only tool that folds to `true`
   in scripts.
3. Should #127's caret bug be checked independently first? Layer 2 makes
   its SHAPE unreachable, but if the store op's loc is wrong for OTHER
   errors too, that survives. Cheap to check, not yet checked.
