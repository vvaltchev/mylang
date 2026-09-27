# Immutable views — a design TODO (not a plan)

**Status: idea, raised by the maintainer on 2026-09-25. Not designed.**
Filed in `plans/language-deferred.md`; this file only introduces the
subject so it is not lost.

## The idea

A way to hand code a **read-only view** of a container that is not
itself `const` — and to check at **compile time** that the code
receiving it cannot mutate it: not directly (`a[i] = v`, `append(a, x)`,
`erase`, `sort` in place, ...), and not by passing it on to a function
that might.

The motivating case is a builtin callback. `sort(arr, cmp)` hands the
comparator elements of an array it is sorting in place. The comparator
is arbitrary script code, so today the only defence against it
mutating `arr` mid-sort is a RUN-TIME guard after every comparison
(#49: `sort()` raises `InvalidArgumentEx` if the comparator changed
the array's length or storage). That guard costs ~5% on
34_sort_custom_cmp. `map`/`filter` pay a similar per-step length
re-read, and `foreach` a per-iteration shift check (#53) unless
codegen can prove the body inert.

If the callback could only ever see the container through an
immutable view — and the compiler proved it — those run-time checks
would disappear: the mutation would be a compile error, not a
defined run-time exception.

## What already exists to build on

- **Deep read-only values at run time**: `const` containers carry a
  `readonly` flag on the shared object; every write path refuses it.
  That is a run-time property of the VALUE, not a static property of a
  reference, and it is all-or-nothing per object.
- **`func_mutates_input`** (resolver.cpp): the taint analysis that
  decides whether a function mutates a reference parameter — what
  makes `pure` mean "no observable side effects".
- **The #93 parameter escape analysis** (`stamp_noescape_params`): a
  transitive, fail-closed, per-parameter fact over the call graph,
  using the callee-set analysis to name callees.

A compile-time "does not mutate parameter k" fact is close to both of
those, and would likely share their machinery and their fail-closed
rules (an unknown callee or node shape = "may mutate").

## Open questions (for when this is picked up)

- **Surface syntax**, if any: a parameter annotation (`func f(view
  array<int> a)`), a type modifier, or purely inferred with no syntax.
- **What a view forbids**: element writes only, or also growth; is a
  nested container reached through it also read-only (deep vs
  shallow)?
- **Where the guarantee comes from**: the callee's declaration, or a
  whole-program proof at each builtin call site (the comparator is
  usually an inline lambda, so the latter may cover most cases without
  new syntax).
- **Interaction with `dyn`, aliases and captures**: a view stored in a
  captured variable, or into another container, must not become a
  mutable path back to the original.
- **Relation to `const`**: whether a view is just a non-owning
  `const`, and what that means for COW and slices.
- **RULE 1 / RULE 2**: every engine must agree, and a program that
  compiles today must still compile unless it really mutates through
  a view.
