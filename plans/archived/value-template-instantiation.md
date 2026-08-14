# Value-used template instantiation (top-10 #10's real fix)

Status: DONE (2026-07-17). Maintainer-approved; landed with the full
test-obligation set. MEASURED (full-suite interleaved A/B):
76_funcval_dispatch 0.105→0.067s (**0.68x my/py, was 1.05-1.12x — the
last CPython-losing bench now wins**; its callee bodies went from 6
boxed ops to 4 typed ones), suite VM-wall geomean 0.996, my/py 4.87x.
KNOWN GAP (v2 candidate): value-instantiation is INPUT-LOCAL in the
REPL — an input-1 array called indirectly from input 2 keeps the boxed
base (correct, unoptimized); pinned by the `repl:` test.

## The problem

A NAMED template that is only ever VALUE-used — stored in a container /
var and called INDIRECTLY (`ops = [add_op, sub_op]; fn = ops[i%2];
fn(st, i)` — bench 76's shape, phonebook's `cmd_*` dict) — is never
instantiated: `instantiate_round` redirects DIRECT calls only. The
un-typed BASE body runs for every indirect call, fully BOXED in both
engines (76's callee bodies are `subscript.v` + `bin.v ; boxed`).
CPython's dynamic dispatch is its home turf; we pay dispatch AND boxing.

## The design: finfo-carrying Func types + uniform-signature instances

1. **The `Func` StaticType carries a bounded FUNC-INFO SET** (`finfos`,
   ≤4 members): `func_static_type(fi)` seeds {fi}; `join` unions the
   sets (over-cap → drop the set + mark members ESCAPED). The set rides
   the EXISTING lattice propagation for free: `[add_op, sub_op]` joins
   the two, `ops[i]` reads the element type, `fn` accumulates it — the
   indirect call's callee type names its candidate templates with no
   new dataflow machinery.
2. **ESCAPE marking** (`FuncInfo::value_escaped`): any flow that DROPS
   the set marks its members escaped — contribution into a
   dyn-resolved symbol / dyn param / `runtime()`, a dict-value or
   struct-field contribution (v1: not tracked through those), a
   builtin arg (map/filter callbacks keep today's boxed-base behavior),
   an over-cap join. An escaped template is NEVER value-instantiated:
   an untracked call site could reach the typed instance with a
   mismatched signature, and while the M8 safety net would make that a
   runtime TypeError rather than corruption, it would still CHANGE a
   working program's behavior — so escape = keep the boxed base.
3. **The instantiation rule** (a sub-pass in the instantiate outer
   loop): for each un-escaped, value-used template T, collect every
   INDIRECT call whose callee type's finfo-set contains T. If ALL of
   them have ONE settled, arity-legal signature S: get-or-make the
   T@S instance (the existing `make_template_clone` + `tmpl_cache`
   path — same cache key, same `$N` naming, same `display_name`) and
   REDIRECT EVERY VALUE-USE Identifier of T to the instance (the same
   id_sym-rebind mechanics `instantiate_round` uses for callees).
   Different signatures across sites → leave everything alone (the
   base keeps running, correct as today).
4. **After the redirect** the base is no longer value-used, so the
   existing dead-template-base exclusion (`is_template_base`) applies
   and the indirect calls run the TYPED instance chunk natively.

## Observable-semantics audit

- Identity/equality: ALL value uses redirect to ONE instance, so
  `ops[0] == add_op`-style identities hold (both sides are the
  instance). Direct calls redirect per-signature as before,
  independently.
- Backtraces: `display_name` keeps the base name (existing clone rule).
- `typestr`/`type()` of the value now render the INSTANCE's typed
  signature — the same accepted class of change as direct-call
  instantiation (`:show dot$0` renders typed).
- `-nti`: no instantiation, unchanged.
- REPL: instances pin/GC exactly like call-redirected ones; a value
  use inside a function body sets `has_func_consumer`.

## Test obligations (the optimizations-must-generalize bar)

- The bench-76 shape (array of two templates, uniform calls) → typed.
- NON-uniform signatures (two call sites, different types) → NO
  instantiation, program byte-identical.
- ESCAPE cases: template value into a `dyn` var then called with a
  DIFFERENT signature → still works (boxed base; the escape mark must
  catch it). Dict-stored (phonebook-shape) → NOT an escape after all:
  the finfo set rides a dict-literal value join intact (both members are
  Funcs), so those sites ARE attributed and the templates DO instantiate
  when the signature is settled. (This line used to claim "v1 unchanged
  (escape)"; the implementation tracks further than the design text
  assumed. What actually protects the phonebook shape is the
  uninformative-signature decline below, not an escape.)
- Direct + value use MIXED (a direct float call + uniform int value
  calls) → both instances coexist.
- REPL cross-input: define the templates + array in one input, call
  indirectly from a later one.

## Fix (2026-07-29): decline an UNINFORMATIVE signature

`samples/phonebook` regressed the moment this feature landed: its `cmd_*`
templates live in a dict and are called `cmdfunc(data)` with
`var data = {}` — a container the CALLER has no element info for, because
the only writes go through a *callee's* reference param (`cmd_add`'s
`data[n] = ...` contributes to that PARAM's symbol; nothing flows back to
main's `data`). So the attributed signature was the BOTTOM
`dict<none,none>`, uniform across the sites → both templates were
instantiated on it, the clones' params were seeded with it, and
`cmd_view`'s `foreach (var k, v in data)` typed `k`/`v` as `none` — making
its `print(k+":", join(v, ","))` a compile-time `NullabilityEx` on a
program that is perfectly correct at runtime. (Pre-feature the param was
plain `dyn` and the base body was never checked.)

`value_instantiate_round` now treats such a signature like a `dyn` one:
not settled, so **no instantiation — the boxed base keeps running**. The
predicate is `Inferencer::type_has_bottom_elem` (an `array`/`dict` whose
elem/key/val is the bottom `none`, recursively). It is a DEFER: a later
fixpoint round that settles the element type still gets the instance, and
a real signature (bench 76's `array<int>, int`) is unaffected — verified
by `-T template` on both. A WRITER body would have repaired its own param
type by contribution; a READER body cannot, which is why declining is the
only sound answer here rather than "instantiate and let the body fix it".

Regression tests: `value-templates decline an empty-container signature
(phonebook)` + the array-stored twin (src/tests.cpp), both engines.

Note the DIRECT-call `instantiate_round` has the same blind spot and it
PRE-DATES this feature (`func v(d){foreach(k,e in d) print(k+":");}` called
directly with an empty `{}` has always been rejected). Left alone here: a
direct container arg is usually written by the callee (which repairs the
type), so gating it would cost real monomorphization on the common
fill-a-fresh-array shape. A candidate v2 item — the principled fix is to
make an element READ out of a bottom container yield `dyn`, not `none`,
which is a lattice change (it would turn `var x = empty[0]` into a
`DynRequiredEx`) and needs maintainer sign-off.

## #149 (2026-08-11): base reachability is a CLOSURE, not the flag

The dead-base exclusion (`is_template_base`) was keyed off the name
sym's `value_used` alone - and that misses ONE HOP: a value-used base
runs its ORIGINAL body, whose direct calls were never redirected to
instances (the check/instantiation passes skip template bodies), so
every template that body names is itself runtime-reachable, and so on
transitively. `var dyn g = aa;` kept aa's base compiled, but aa's base
body calls bb - whose base was excluded as dead - and the indirect
g() call reached a chunk-less bb one hop in: the post-teardown
ML_CHECK abort in a script (or, under ASSERTS=0, a tree-walk of a
freed AST). The -rt harness RETAINS its AST, so no `tests` entry can
see this - the net is tests/functional/dyn_template_base.my
(corpus_diff spawns the real binary, teardown included; the sabotage
run showed 14/15 with the abort named). The fix is a fixpoint in the
finalize pass: seed the kept set with the value-used bases, then any
IDENTIFIER naming a template inside a kept base's body (call or value
use; nested lambdas descended - the whole subtree runs with the base)
keeps that template too. Over-keeping (a body-local shadowing a
template's name) costs a compiled chunk, never correctness.
