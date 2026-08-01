# Function templates (monomorphization) — design plan

> **STATUS: v1 BUILT (all CI green: Linux/macOS/Windows/Coverage; GCC + clang,
> debug + opt + sanitized).** A NAMED function with ≥1 un-annotated, non-`dyn`,
> non-`opt` parameter is a template: not checked in isolation (D2 structural-
> only), instantiated per call-site signature as a typed clone the concrete-
> function inference + M8 + eval handle (D3), with the call redirected like the
> `$specN` clones. `dyn` is never *required*. Decisions: D1 un-annotated non-
> `dyn` param ⇒ template; D2 structural-only never-called; D3 monomorphized
> eval together.
>
> **Also built (v1.1):**
> - **opt-param templates** — a template is now any named function with ≥1
>   *template param* (un-annotated, non-`dyn`, **non-`opt`**); `opt`/typed/`dyn`
>   params coexist and `join` within each clone, so `func f(a, opt b)` is
>   generic over `a` while `b` joins. (`func f(opt x)` with no template param
>   keeps the join model.) The signature is keyed by the template params only.
> - **var-bound lambda monomorphization** — a non-capturing anonymous lambda
>   bound to a **write-once, calls-only** var is promoted to a template
>   (`mark_lambda_templates`, after the structural pass; the inferencer gained
>   per-symbol write/value-use bookkeeping). So `var id = func(x)=>x; id(1);
>   id("s")` works. A value-used / capturing / reassigned lambda stays join.
> - **D4** — a per-template instantiation cap (64 signatures) past which calls
>   run dynamically, with a one-time stderr warning.
>
> **Also built (v1.2):** **cross-input REPL monomorphization** — a template
> defined in a PRIOR REPL input now instantiates in later inputs (it used to
> evaluate dynamically). This took finding a real **MSVC-only, non-deterministic
> (address-dependent) UB**, done by pushing super-heavy instrumentation through
> CI: `id_sym`/`func_of_decl` are keyed by node POINTER and persist for the
> session; a fresh input node can reuse a freed node's address, and
> `walk_struct`'s `if (!id_sym.count(id))` guard then KEPT the stale entry —
> binding an input's call CALLEE to a *prior clone's* symbol, so the call was
> never instantiated and was checked against the wrong instance. No local config
> reproduced it (GCC + clang × debug + opt × ASan + UBSan), and it flipped
> Debug↔Release run-to-run. Fix: always re-resolve in `walk_struct` (it visits
> each node once) + clear the clone subtree's `id_sym`/`func_of_decl` in
> `make_template_clone`. All CI green.
>
> **Deferred / deliberate v1 boundaries:**
> - **D5 / template-as-a-value already works** (correctly typed): `var a =
>   map(inc, xs); a[0]+1` type-checks. Monomorphizing the callback is near-zero
>   value (`map` builds a general array + calls through its dynamic loop), so it
>   is intentionally left on the dynamic path. Inline lambda callbacks too.
> - The general value-passed-lambda + capturing-lambda cases stay join.

## 1. The problem & the model

Today a function's parameter gets **one** static type: the `join` of every
call-site's argument type (`contribute_arg`, `inferencer.cpp`). Two consequences
the maintainer flagged as wrong:

- A function **defined but not called** has unconstrained params → they finalize
  to `dyn`, and the body is type-checked *in isolation* with `dyn` params. A
  local derived from them (`var t = x + y`) is then `dyn`, which the
  mandatory-`dyn` rule rejects — so a perfectly normal generic helper
  `func g(a){ var r = a + 1; return r; }` is refused unless you write
  `var dyn r` or annotate the param. (And before a fix, the local even inferred
  `none`, so the error was a bogus *nullability* message.) Inconsistent, too:
  `func g(a) => a+1` and `func g(a){ return a+1; }` are accepted — only the
  local-binding form trips.
- A function called with **two different types** is a hard error:
  `func f(x){ return x; } f(1); f("s");` → `TypeMismatchEx: 'x' has type 'int'
  but is assigned 'str'` (the join of int and str conflicts).

**The model (the maintainer's):** a function *without explicit parameter types*
is a **template**, like in C++ — not a function, but a thing **instantiated per
call-site signature**. Then:

- We **never need to require `dyn`** for an un-annotated param. `dyn` becomes
  just the case where *one* instantiation serves all argument types (an
  explicitly dynamic parameter), not something inference forces on you.
- `f(1)` and `f("s")` make **two instantiations** (`f<int>`, `f<str>`), each
  type-checked independently — no conflict.
- A **never-instantiated** template is never type-checked against concrete
  types (C++ two-phase: phase-1 structural checks only), so a defined-but-unused
  generic helper produces **no spurious error**. This is exactly the REPL
  workflow: define `func g(a){…}` in one input (a template, unchecked), call
  `g(5)` in the next (instantiate `g<int>`).

This is **monomorphization** restricted to the type-checker: the tree-walking
evaluator is already dynamic, so v1 changes *what gets type-checked and how*,
not how code runs. Speed-specialization (a compiled body per instantiation) is a
later, optional increment that reuses the existing `$specN` / M8 machinery.

## 2. What is a template

A function is a **template** iff it has **≥1 un-annotated, non-`dyn`
parameter** (a *template parameter*). Otherwise it is a **concrete function**,
inferred/checked exactly as today.

- `func f(a, b)` → template over `a, b`.
- `func f(int a, b)` → template over `b`; `a` is fixed `int`.
- `func f(int a, int b)` → concrete (no template params).
- `func f(dyn x)` → **concrete**: `dyn` is an explicit "one instantiation, any
  type" — *not* a template parameter. So `func f(dyn x){ var r = x; … }` keeps
  today's meaning (and a derived `dyn` local there still wants `var dyn`, since
  the user opted into dynamic explicitly).

This keeps `dyn` meaningful (the deliberate dynamic escape hatch) while making
*un-annotated* the lightweight generic spelling.

## 3. Two-phase checking

Mirrors C++ templates:

- **Phase 1 — definition (always).** Structural validation only: names resolve,
  `break`/`return` placement, arity of *concrete* callees, syntax. No type
  errors that depend on a template parameter are raised here. A never-called
  template gets *only* this. (Open decision D2: do we additionally run a
  best-effort "all-`dyn`" pass to catch obvious non-dependent body errors? See
  §7.)
- **Phase 2 — instantiation (per call signature).** At a call
  `f(arg₁…argₙ)` whose callee is a template, compute the argument types, bind
  the template params to them (typed params are checked against their args as
  today), and **infer + check the body** under that binding. The call's result
  type is that instantiation's return type. Instantiations are **cached** by the
  template-param signature, so `f(1)` twice instantiates once, `f(1)`+`f("s")`
  twice.

`mandatory-dyn` no longer fires on a template-param-derived local: under an
instantiation the param is concrete (`int`/`str`/…), so `var r = a + 1` is
`int`/`str`/…, never `dyn`. It still fires on a genuinely-`dyn` local
(`var x = runtime(…)`, an explicit `dyn` source) and on top-level `dyn` vars.

## 4. Inferencer changes (the core work)

Today: one `FuncInfo` per function; `contribute_arg` joins each call's arg type
into the single param `TypeSym`; the body is inferred once in the global
fixpoint. Templates need **per-instantiation** param types and body inference.

Proposed shape:

- Mark each `FuncInfo` `is_template` (computed in the structural pass from the
  param annotations).
- **Concrete functions**: unchanged — global fixpoint, one body inference.
- **Template functions**: *not* part of the global symbol fixpoint for their
  params/locals/return. Instead, an **instantiation worklist**:
  1. Run the main fixpoint over concrete code so call-site argument types
     settle (a call to a template contributes nothing to a shared param now; its
     *result type* is supplied by instantiation — see recursion below).
  2. For every call whose callee is a template, derive the template-param
     signature from the (now-settled) arg types and enqueue an instantiation if
     that signature is unseen.
  3. **Instantiate**: clone the body's symbol set (params + locals + a return
     accumulator) into a fresh per-instantiation scope, seed the template params
     to the signature types, run a *local* fixpoint + check over the body, and
     record the return type for that signature. New call sites discovered inside
     (calls to other templates, or recursion) feed back into the worklist until
     it drains.
  4. A call's result type = its instantiation's recorded return type.
- **Caching**: keyed by `(FuncInfo*, signature)` where `signature` is the tuple
  of template-param `StaticType`s (structural equality via the existing
  `StaticType`
  machinery). Bounded (open decision D4: cap on instantiations per template to
  avoid blow-up on deeply polymorphic recursion).

**Refinement (the clean mechanism).** A clone doesn't need its param types
*seeded or pinned*: because clones are **deduped by signature**, each clone has
exactly one signature's worth of (redirected) call sites, so its un-annotated
params simply **accumulate to that signature** through the ordinary
concrete-function path — no conflict, no special casing. So instantiation is:
clone the template (synthetic id + `display_name`), `walk_struct` it, set its
`FuncInfo::is_template = false` (it is now concrete), insert it at the root
block, and **redirect** the call's callee to the clone. The next fixpoint round
infers the clone's body from its sig-typed params, exactly like any concrete
function — which also gives M8 / flat arrays for free.

In the fixpoint, an **un-instantiated template is skipped**: `accumulate` does
not descend its body and does not `contribute_arg` to its params; the check pass
does not validate its body (structural-only). The instantiation outer loop runs
between the fixpoint and finalize: collect template-call signatures from the
settled arg types, instantiate+redirect new ones, re-run the fixpoint; repeat
until no new signatures (bounded by D4).

Recursion / mutual recursion fall out of dedup: instantiating `g<int>` whose
body calls `g(n-1)` (int) finds, on the next collection pass, that `g<int>`
already exists → it redirects the recursive call to the same clone (self-
recursive). Mutual `a→b→a` converges the same way through the shared
instantiation cache.

**Risk to control: the template path applies to a LOT of existing code** (every
`func f(x) …` with an un-annotated param). Concrete functions (all params
typed/`dyn`) must stay byte-identical. For a single, consistently-typed call
site the old join already produced the right answer, so an option to **minimize
divergence** — only diverge from today where today is wrong (0 call sites →
structural-only; conflicting call signatures → per-signature clones; a single
consistent signature → leave the join) — is on the table if full instantiation
churns the 944-test suite too much. The full-instantiation form is the clean
target; the minimal-divergence form is the fallback. Either way the suite is the
guard.

## 5. The unifying mechanism: an instantiation IS a typed clone

Type-checking and monomorphized eval come from **one** mechanism (D3): each
instantiation is a real **clone of the template body whose params are annotated
to the signature** — `g(a)` called `g(5)` ⇒ a clone `func g$int(int a){…}`. The
clone is a **concrete function**, so the *existing* machinery does all the rest
for free: the inferencer checks it (typed params ⇒ concrete locals, the precise
errors), `resolve_names` slots it, `specialize_types` gives it M8/flat-array
speed, and the evaluator runs the specialized body. Call sites are
**redirected** to their clone like the const-arg `$specN` clones (insert at root
block's front, dedupe by `(template, signature)`, `display_name` keeps
backtraces showing `g`). So there is no "dynamic v1, fast v2" split — an
instantiation is monomorphized from the start.

The original template `FuncDeclStmt` is kept (structural-only, never directly
called at runtime since every call is redirected) so later code — and, in the
REPL, later inputs — can still instantiate new signatures from it.

## 6. REPL interaction

Fits the faithful per-input pipeline directly. A template defined in input *N*
is structurally checked and its `FuncInfo`/body **pinned + retained**. A call in
input *M > N* triggers instantiation *then*. So `func g(a){ var r=a+1; return
r; }` (input 1, a template, no error) then `g(41)` (input 2, instantiate
`g<int>`) → `42`. The instantiation cache persists across inputs on the
`ReplInfer`'s `Inferencer`. `undef(g)` drops the template + its instantiations.

## 7. Open decisions (need the maintainer's call)

- **D1 — template trigger.** Confirm: *any* un-annotated non-`dyn` param ⇒
  template; `dyn` params are *not* template params (stay concrete/dynamic). (My
  recommendation, §2.)
- **D2 — never-instantiated body checking.** Pure C++ two-phase
  (**structural-only**, so an unused template's type errors surface only when
  first instantiated)? Or *also* a best-effort all-`dyn` pass at definition to
  catch obvious non-dependent mistakes (at the cost of some false positives the
  template model is meant to avoid)? Recommendation: **structural-only** — it is
  the whole point ("never require dyn, instantiate on use") and matches the REPL
  define-then-call flow.
- **D3 — scope of v1.** Type-checking monomorphization only (eval stays
  dynamic), with v2 (specialized bodies) deferred? Recommendation: **yes**,
  ship correctness first.
- **D4 — instantiation bound.** A cap (e.g. 32 signatures/template) beyond which
  a template falls back to a single `dyn` instantiation, to bound compile time
  on pathologically-polymorphic recursion? Recommendation: **yes**, with a
  `log`/diagnostic when hit.
- **D5 — template passed as a value** (`map(f, xs)` where `f` is a template, or
  `var h = f;`). No direct call signature at that site. Options: (a) instantiate
  from the *expected* callback/param type when known (HO builtins already feed
  element types into callback params — §`callee_funcinfo`); (b) fall back to a
  single all-`dyn` instantiation; (c) error. Recommendation: **(a) then (b)** —
  use the known expected type if there is one, else one `dyn` instantiation.

## 8. Increments (once decisions land)

1. **Structural template marking** — compute `is_template`; route concrete
   functions through today's path unchanged (no behavior change; pure
   plumbing + tests that concrete code is identical).
2. **Per-call instantiation for type-checking** — the worklist, per-signature
   body inference, caching, recursion guard; remove the join-into-shared-param
   for templates; drop mandatory-`dyn` on template-param-derived locals. This is
   the bulk and delivers the user-visible behavior.
3. **REPL wiring** — persist the instantiation cache on `ReplInfer`; instantiate
   at cross-input call sites; `undef` eviction.
4. **(v2, optional) monomorphized eval** — per-instantiation `$specN`-style
   clones for speed.

Each increment keeps `-rt` green; the existing typed-function tests become the
"concrete functions unchanged" guard, and new tests cover multi-type
instantiation, never-called templates, recursion, and the REPL flow.
