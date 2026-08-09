# Deleting the undefined-name error class (layers 0, 1, 2)

Status: **PLANNED, not started.** Agreed with the maintainer 2026-08-07.
Supersedes the "make the engines agree" framing of #126/#127/#128.

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
