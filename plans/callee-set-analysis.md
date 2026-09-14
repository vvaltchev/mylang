# THE CALLEE-SET ANALYSIS: one answer to "which function is this?"

**Status: INCREMENTS 1-3 LANDED (2026-08-28), 4 NOT STARTED.** A
STRUCTURAL change, not a performance one - the maintainer's framing when
agreeing to it. The measured payoff on today's corpus is ~2.8% on one
bench (#115); the argument is that FOUR partial analyses answering
slices of one question become ONE, and that the answer becomes able to
say "I do not know".

## The category error this fixes

There are two different questions about a callable value:

 - **SHAPE** - how do I emit the call, the window, the return?
   Asked by the JIT. Answered by the `Func` StaticType.
 - **IDENTITY** - is it always the SAME function? Asked by
   devirtualization. Answered by a callee set.

Type equality is CORRECT for the shape question, and `join`'s
`static_type_equal` shortcut is correct with it: N functions assignable
to one variable share one shape, and one answer is what the emitter
needs. Nothing here proposes changing that.

`StaticType::finfos` asks the IDENTITY question off the SHAPE object.
That is the error. Two distinct functions routinely have equal types -

```C#
func sq(x)  => x * x;
func neg(x) => 0 - x;
print(typestr(sq));    # func(?)->?
print(typestr(neg));   # func(?)->?     <- identical
```

- so every shape-level equivalence is a place identity dies. `join`'s
`if (static_type_equal(a, b)) return a;` was the first one found (fixed
in 4a1c246 by making the Func arm union the sets), and there is no
argument that it is the last, because the whole class is "operations
that treat equal types as interchangeable" - which is what a type system
is FOR.

**⛔ SO 4a1c246 IS REVERTED BY THIS WORK.** It teaches `join` to preserve
information that should not be on a type. Leaving it after the
replacement lands means two mechanisms claiming the same fact.

## The deeper flaw: no ⊤

`finfos` cannot distinguish **"nothing flows here"** from **"I have no
idea"**. Both are the empty set. That is why:

 - `escaped_finfos` / `FuncInfo::value_escaped` exist at all - a
   side-channel ledger bolted on to carry the "unknown" that the set
   itself cannot express;
 - the ledger is *structurally* insufficient, which #115 hit: it records
   members a join DROPPED, and says nothing about members that never
   ENTERED;
 - `finfos.size() == 1` was never a proof. #115 needed a UNIFORMITY rule
   on top purely to survive that.

A lattice with a real ⊤ makes "I do not know" a first-class answer, and
every decline becomes a decline for a stated reason.

## What exists today (all four are partial, none share evidence)

**`TypeSym::func`** (inferencer 2643) - covers `var f = <lambda
literal>`. Says unknown by being null.

**`StaticType::finfos` + `escaped_finfos` + `value_escaped`**
(statictype.h 77, inferencer 978) - covers values flowing through
types. Says unknown through a SIDE LEDGER, incompletely (see above).

**`index_func_aliases` / `callee_of`** (resolver 1943, 2008) - covers
write-once aliases, for the step-7 unbound-call prover. Returns null.

**`direct_func_slot` / `slot2fn` / `esc_callback_fn`** (resolver 4168,
3354, 3382) - covers global-slot callees and the escape analysis's
callbacks. Says unknown via `written_slots`, or null.

`esc_callback_fn`'s own note records that the grep it was built from
**misses `map`, `filter` and `sort`** - they reach their callback
through a shared helper not named `builtin_*`. That is one analysis
already known to be wrong about which functions a call can reach.

## The analysis

A monotone least-fixpoint over FUNCTION VALUES ONLY - a far smaller
domain than the type lattice, and the same solver shape the inferencer
already runs.

**Abstract locations** (keyed by PROGRAM LOCATION, never by type):
 - each `TypeSym` - variable, parameter, capture
 - `ret(F)` per `FuncInfo`
 - `elem(S)` / `val(S)` per array/dict-typed symbol
 - each struct field that can hold a function

**Lattice**: `⊥` (nothing yet) ⊑ finite sets of `FuncInfo *` ⊑ `⊤`
(unknown). Cap the set at a small N and collapse to ⊤ past it, so a
pathological program cannot make the sets large.

⛔ **AS BUILT, THE LOCATIONS ARE `Sym` / `Ret` / `Elem(obj)` ONLY** -
one abstract object per container ALLOCATION SITE, with symbols
pointing at objects as well as functions (textbook Andersen). That
replaces the plan's `elem(S)`/`val(S)`-per-SYMBOL sketch, which is
unsound without an alias analysis: two symbols can name ONE container
with no copy the walk models (`f(p)` binds it to a parameter, `[p]`
nests it, a `dyn` alias re-reads it). Letting the OBJECT be the
location makes aliasing fall out of the ordinary `q = p` copy rule.
Struct fields and dict keys/values share their instance's object -
field- and key-insensitive, which costs precision only on a program
that puts different functions in different fields of one struct type.

The opposite simplification - ONE shared "heap" location - was tried
first and is trivially sound but useless: it merges every container in
the program, and would alone drop
`tests/functional/25_factory_closure_param.my` case (6) from a MUST
answer to a 5-way decline. That is now a watched-failing sabotage.

**Constraints**, one walk:

```C#
var f = func(x) => x;      # pts(f) ⊇ { that lambda }
var g = f;                 # pts(g) ⊇ pts(f)
return h;                  # pts(ret(F)) ⊇ pts(h)
var k = mk();              # pts(k) ⊇ pts(ret(mk))
var p = [a, b];            # pts(elem(p)) ⊇ pts(a) ∪ pts(b)
var q = p[i];              # pts(q) ⊇ pts(elem(p))
f = <unanalyzable>;        # pts(f) = ⊤
```

**Query**: `callee_set(expr)` -> a set or ⊤. Exactly one member and not
⊤ is a MUST answer - the only thing that licenses devirtualization or
typing that one function's parameters.

## Increments

Each lands green on the full net battery. The order is chosen so the
first one is measurable and the risky one is last.

**1. THE ANALYSIS + ITS OWN TESTS, CONSUMED BY NOBODY. ✅ DONE
(2026-08-28)** - `src/calleeset.cpp.h`, `#include`d once into
inferencer.cpp, run at the end of `infer_one` after the instantiate
loop. `-dcs` dumps one row per call site
(`direct`/`one`/`many`/`top`/`none`/`builtin`) plus one `dcs-esc` per
escaped function; the `infer: the CALLEE-SET analysis` `-rt` entry
asserts the set per constraint form and a stated ⊤ per sink, plus a
`driver_checks.sh` case (a new CLI flag - `-rt` cannot see the driver).
INERTNESS PROVEN: `-vd` byte-identical on all 126 corpus programs
against a HEAD baseline.

Corpus-wide answers: **48 `one`** (a MUST answer reached through a
value), 14 `many`, **2 `top`**, 1 `none`, 152 `direct`, 767 `builtin`;
21 functions escape.

**2. MIGRATE #115. ✅ DONE (2026-08-28).**
`snapshot_indirect_callees` asks `callee_set(e)` and
`callee_escaped(f)`; `value_escaped` is gone from this consumer. All
six pinned cases in `tests/functional/25_factory_closure_param.my`
print exactly what they printed before, and the migration changes
**0 of 126** corpus programs' `-vd` or output.

⛔ **UNIFORMITY: SURFACED AS A LANGUAGE QUESTION, THEN DELETED ON THE
MAINTAINER'S CALL (2026-08-28).** The plan said it would "dissolve" on
migration. It did not dissolve - it had become a POLICY once `join` was
fixed, and deleting it changes OBSERVABLE OUTPUT, so it went to the
maintainer as a decision with both behaviours measured. He chose
agreement, and it is deleted.

What it was: "every site attributed to a callee must pass the SAME
argument types, or none of them contributes." It never computed a
different answer - it decided whether to answer AT ALL, because
`contribute_arg` is the same function the direct-call path already ran
unconditionally. A named lambda's sites always joined; a
factory-returned closure's route to `contribute_arg` is
`indirect_callee`, and this was the gate on it.

Result: the two spellings of a CAPTURING closure are now
indistinguishable - `int`/`float` sites widen and coerce in both,
`int`/`str` refuses in both. A NON-capturing var-bound lambda is a
TEMPLATE and still monomorphizes per signature; that is a different
feature and untouched. Blast radius: 1 of 123 corpus programs, one
line. Watched: restoring the check fails 10 `-rt` checks and re-splits
the two adjacent lines of `25_factory_closure_param.my` (case (3) and
its new direct-spelling twin (3b), kept side by side so they cannot
drift apart again).

⛔ **DO NOT RESTORE IT TO FIX A REFUSAL.** It was safe to delete only
because the candidate set is COMPLETE: it existed to survive an
under-collecting set, where sites of two DIFFERENT closures were
attributed to one survivor and joining them invented a conflict. If a
valid program is refused now, the candidate set is wrong - fix that.

⛔ **AND THE REAL ANSWER'S FIRST DIVIDEND WAS A SOUNDNESS FIX #115
SHIPPED WITHOUT: THE MULTI-SITE HOLE.** Attributing every size-1 site
is not enough - a closure can be reached BOTH by a site naming it
alone AND by a site that may reach it or another:

```C#
func mk_a(n) => func [n] (x) { return "A:" + typestr(x) + str(n); };
func mk_b(n) => func [n] (x) { return "B:" + typestr(x) + str(n); };
func probe(int k) {
    var only_a = mk_a(1);           # names ONE closure
    var p = [mk_a(2), mk_b(3)];     # ...the SAME FuncInfo as only_a
    var either = p[k];              # may be either
    return only_a(7) + " " + either("str");
}
```

`only_a(7)` typed the parameter `int`, `either("str")` was never
attributed, and the check pass REFUSED the program on a signature the
rule had invented. A callee is usable only when EVERY site whose set
CONTAINS it names it ALONE (`disqualified`). Pinned as case (7) of the
functional test and as an `-rt` row asserting the analysis REPORTS the
second site as `many`.

⛔ **AND CLOSING IT EXPOSED A GAP IN `corpus_diff`**: a program that
stops COMPILING agrees with itself, because the comparison is between
two ENGINES and a compile refusal happens before either runs. The
sabotage made `25_factory_closure_param.my` fail to compile entirely
and the script still said 33/33. It has a **`compile gate`** now (`-nr`
per program). Exit code would not have done it - `samples/gcd`
legitimately exits 1 with a usage message.

Two escape rules were added for this increment's consumer, both
because a ⊤ that a function could be IN must imply that function
escaped: a **`throw`** escapes its value (the catch binding reads ⊤),
and a write to a **REPL-pinned global** escapes (a later input can
redefine it).

**3. MIGRATE `value_instantiate_round`. ✅ DONE (2026-08-28).** It asks
`callee_set` / `callee_escaped`; `StaticType::finfos`,
`StaticTypeArena::escaped_finfos`, `note_escaped_into`, `finfos_add_to`
and `drain_escapes` are DELETED, and 4a1c246's `join` union is
REVERTED - the shortcut is a plain `if (static_type_equal(a, b)) return
a;` again and an `StaticType` describes SHAPE and nothing else.
**0 of 126** corpus programs change `-vd` or output, and
76_funcval_dispatch still gets its `add_op$0`/`sub_op$0` instances (a
vacuity check: byte-identity alone would also be satisfied by the
feature having silently stopped firing).

⛔ **`value_escaped` IS *NOT* DELETED, and that is a correction to this
plan.** It had two producers and only one of them was the ledger. The
one that stays asks a question the analysis deliberately does not
model: is there a value use whose CONSUMERS cannot be enumerated - an
ARG-position use or a capture-list use. `value_instantiate_round`
REDIRECTS every value-use Identifier to the clone, so a use it cannot
FOLLOW would hand the typed instance to a consumer free to call it with
a mismatched signature. **Tracking a value further (which the analysis
does - an argument flows into the parameter) is not the same as being
able to redirect it.** The two gates now sit side by side, each with
its question written down.

⛔ **AND `stamp_proven_params` (C3) HAD TO GAIN `callee_escaped`.** Its
gate leaned on `value_escaped`, half of which came from
`drain_escapes`. Deleting the ledger without replacing that half would
have silently weakened a stamp every unboxed tier is built on - the
exact shape of "an optimization that only affects SPEED has no
correctness oracle", except here it affects a PROOF.

⛔ **A TEST WRITTEN AGAINST BEHAVIOUR OUTLIVED THE MECHANISM.** The two
`-rt` rows added for the `join` completeness bug assert `271s` and
`a:dyn` - values, not internals - so deleting the entire mechanism they
were written against required no edit to either, and their coverage
survived. A test written against `finfos` would have been deleted with
it.

Compile time: flat (0-3%, within noise) even though `cs_run` now runs
once per instantiate round - it must, because
`value_instantiate_round` CONSUMES the sets and each round redirects
call sites to fresh clones, so the previous round's answer describes a
tree that no longer exists.

**4. MIGRATE THE RESOLVER'S THREE.** `callee_of` (the step-7 prover),
`slot2fn` / `direct_func_slot` (devirtualization), `esc_callback_fn`
(the escape analysis). ⛔ THIS INCREMENT IS MEMORY-SAFETY-CRITICAL: #93
/ #94 make a false "safe" a USE-AFTER-FREE, and the escape analysis
fails closed by design. Every rule there is pinned by a
`param_escape_analysis` row that must be re-watched failing against the
new source. Doing 1-3 and stopping is a legitimate outcome; it gets the
correctness, and 4 is what collapses the duplication.

## Placement, and why it is the hard part

Not the solver - the sequencing.

 - It needs POST-INSTANTIATION types. A call to a template DEFERS by
   design, so a factory's return type does not exist until
   `instantiate_round` has made the clone and redirected the call. #115
   proved this the expensive way: the snapshot placed after the first
   `run_fixpoint` found ZERO sites and the rule was silently inert.
 - The resolver's consumers run LATER (`resolve_names`, then the
   inliner, then `devirtualize_direct_calls`, then
   `stamp_noescape_params`), and the inliner REWRITES the tree under
   them.
 - So: run it at the end of `infer_one`, after the instantiate loop; and
   for increment 4, re-run or incrementally update it after the inliner,
   because a spliced body is new call sites.

**Open**: whether one run serves both sides or increment 4 needs its own
re-run. Decide with a measurement (does re-running change any answer on
the corpus?), not by argument.

**Increment 1 placed it at the end of `infer_one`, after
`instantiate_to_fixpoint()`** - for #115's reason exactly: a call to a
template DEFERS by design, so a factory's return type does not exist
until instantiation has made the clone and REDIRECTED the call. Run
before that loop, the analysis walks call sites that no longer exist
and misses the ones that replaced them.

## The sink audit

The ⊤ assignments are the correctness surface, and the same enumeration
`esc_builtin_transparent` already got wrong once. Every one of these
must assign ⊤ unless proven otherwise:

 - a builtin that may STORE its argument or INVOKE it (start from
   `esc_builtin_transparent` / `esc_builtin_no_invoke`, and re-derive
   the invoker list by the awk over `VmInvoker`/`eval_func` that
   CLAUDE.md quotes - remembering it MISSES map/filter/sort);
 - a capture list (a closure snapshots by value);
 - a struct field, a dict value, a container reached through `dyn`;
 - the REPL's open world (a global can be redefined between inputs);
 - a `.myv` image's pool values (a const array may hold a FuncObject -
   see const_pool_func.my);
 - any AST node shape the walker does not know - the `esc_known_shape`
   rule, verbatim: an unknown kind HIDES occurrences, and a hidden
   occurrence is how a wrong "safe" happens.

## How it is tested

⛔ THE ENGINE DIFFERENTIAL IS BLIND TO ALL OF THIS. A better-analysed
program computes the same answers (see *Testing an AST TRANSFORM*). The
oracles are:

 1. **the `-dcs` dump** - assert the SET, per constraint form and per
    sink. This is the primary net and it is what increment 1 exists to
    make possible;
 2. **`typestr`** for the #115 consumer - the parameter's inferred type,
    scriptable and engine-independent;
 3. **`-vd` byte-identity** for the value-template and devirtualization
    consumers: a migration that changes no decision must change no
    bytecode;
 4. **the 124-program compile+output sweep** against the pre-change
    binary, which is what caught #115's false refusal;
 5. **watched-failing sabotage per rule**, especially every ⊤ sink -
    a sink that stops assigning ⊤ must fail a test, or the analysis
    silently starts claiming knowledge it does not have.

## What this does NOT do

 - It does not change the JIT's call emission. That asks the shape
   question and already reads the type.
 - It does not make `join` or `static_type_equal` identity-aware. The
   opposite: it removes the reason anyone wanted them to be.
 - It is not a general points-to analysis. Function values only; no
   aliasing of containers, no field sensitivity beyond "this field can
   hold these functions".

## What increment 1 actually found

Three bugs, ALL in the unsound direction (a wrong MUST answer, not a
lost optimization), and all three found by the `-dcs` dump run over the
corpus rather than by any test written in advance. That is the argument
for building the dump in the same increment as the analysis.

1. **`append` IS A WRITE, not an opaque builtin.** Treating every
   builtin as a black box loses the STORE, so

       var fns = [];
       for (var i = 0; i < 5; i++) append(fns, mk(i * 100));
       foreach (var fn in fns) tot += fn(1);      # <- ⊥ !

   left the array empty and answered ⊥ - "no function value can reach
   here" - for a site that reaches five closures. ⊥ is a MUST answer.
   FIXED with an allowlist INVERTED from `esc_builtin_transparent`'s: a
   builtin handed a container makes its contents UNKNOWN unless the
   write is modelled exactly, so a missing entry costs precision and
   can never cost soundness.

2. **`None` is "NOT PINNED YET", not "empty"** - the inferencer's own
   defer-on-Unknown/None invariant, met from a new direction. The
   cheap precision gate ("can a function be in this expression at
   all?") answered *no* for the LITERAL in `var fns = [];`, whose own
   type is `array<none>` even though the variable is `array<func>` -
   so no abstract object was ever allocated for the commonest
   container initialiser in the language.

3. **A BAKED CONST VALUE IS NOT IN THE TREE, so it is walked as a
   VALUE.** `const OPS = [sq];` folds to ONE `LiteralObj` at parse time
   (`const_pool_func.my`'s shape) - and so does `var a = [];`. A
   blanket ⊤ is sound but makes that untraceable, so `cs_eval_value`
   chases the FuncObjects out through `desc->decl`, ⊤ only for what it
   cannot name. It resolves `NESTED[0][0]` to `inc` through two levels.

Plus one UBSan catch (`LiteralDict`'s elements are `LiteralDictKVPair`,
not `Construct`, so the plain `MultiElemConstruct<>` downcast is a
different type) and one structural decision recorded rather than
papered over: `cs_eval`'s builtin-callee branch is PROVEN REDUNDANT -
deleting it fails no test, because a builtin name has no `TypeSym` and
the generic path reaches the same ⊤ - so it carries an `ML_CHECK`
saying so, the same treatment #93's reassignment guard has.

**Watched failing, one sabotage build per rule:** None-is-unpinned ->
false; append/push not modelled; one shared heap instead of allocation
sites; LiteralObj back to a blanket ⊤; builtin arguments do not escape;
a catch binding is not ⊤; the flag falls through and `-dcs` runs the
program; `cs_ran` forced false. Deleting the `cs_run` CALL fails the
BUILD (`-Werror=unused-function`), which is a free structural guard
against the pass going dead.
