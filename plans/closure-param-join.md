# Typing a closure's parameters from EVERY call site that may reach it

Status: **IDEA, NOT SCHEDULED** (maintainer, 2026-09-28: "a very nice
advanced optimization... we might do it in the future"). It is a
LANGUAGE change, not only an optimization: it changes which programs
compile and what `typestr` of a parameter prints. Decide it as one.

## The shape

    func make_op(k) {
        if (k % 3 == 0) { var base = k * 10;
            return func [base] (x) { return base + x; }; }
        var factor = k % 7 + 1;
        return func [factor] (x) { return factor * x; };
    }
    var f = make_op(i);
    s = (s + f(i)) % 1000000007;

`x` is un-annotated in both closures, and a closure returned from a
factory cannot be named by the inferencer, so its parameters are typed
only through #115's rule (`snapshot_indirect_callees`, inferencer.cpp):
a call through a variable contributes its argument types to the closure
it reaches.

## Today's rule (gate (c) in CLAUDE.md, *THE FINFO SET NOW TYPES A
## FACTORY-RETURNED CLOSURE'S PARAMETER*)

A call site types a closure's parameters only if its callee set
(`callee_set(e)`, the #116 analysis) names THAT closure and nothing
else - and every site whose set CONTAINS the closure must name it
alone. `f(i)` above reaches both closures, so it types neither: both
`x`s finalize to `dyn`, `base + x` runs boxed, and the #97 two-way
value splice and the tag scalar replacement (increment 5 follow-up C)
never reach bench 99 as written. The bench annotates `int x` to get
past it.

## The proposal

Let a site whose callee set is `{A, B, ...}` (not ⊤) contribute its
argument types to EVERY member, and let the fixpoint's join decide,
exactly as it already does for a lambda the inferencer can name (gate
(b) - uniformity - was deleted on the same argument, 2026-08-28).

Sound in the sense the join needs: every closure in the set may really
be called with those arguments, so each one's parameter must admit
them. The set must be COMPLETE for this to hold, which #116 provides;
⊤ still contributes nothing (and the members it absorbed are already
`callee_escaped`).

## What it changes, script-visibly

- `typestr(x)` inside such a closure prints the joined type (`int`)
  instead of `dyn`.
- A joined numeric parameter coerces at the bind (an `int` passed where
  another site passed a `float` binds as a float - #38 C's stamp).
- Conflicting sites become a COMPILE refusal (`TypeMismatchEx`) where
  today the program compiles with `x: dyn` - the same answer a
  directly-bound capturing lambda already gives (the "two spellings are
  indistinguishable" principle of gate (b)).

## Before building

1. Measure the blast radius over bench/my + samples + tests/functional:
   compile outcome AND output, per program (the way gates (b)/(c) were
   landed).
2. Pin both directions in `tests/functional/25_factory_closure_param.my`
   style: a two-candidate site typing both members; a conflict that is
   now refused; a ⊤ site that still contributes nothing.
3. Re-measure bench 99 WITHOUT the `int x` annotation - it should then
   take the two-way splice and the tag SRA on its own.
