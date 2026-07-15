# Value-used template instantiation (top-10 #10's real fix)

Status: IN PROGRESS (2026-07-17). Maintainer-approved.

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
  catch it). Dict-stored (phonebook-shape) → v1 unchanged (escape).
- Direct + value use MIXED (a direct float call + uniform int value
  calls) → both instances coexist.
- REPL cross-input: define the templates + array in one input, call
  indirectly from a later one.
