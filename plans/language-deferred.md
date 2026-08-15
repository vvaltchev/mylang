# Language & semantics — deferred (needs a maintainer decision)

Items that change **what MyLang means**, not how fast it runs. Each was
designed (some maintainer-confirmed), deliberately not built, and would
otherwise have been buried when its plan was archived on 2026-08-13.

This file is deliberately separate from
`plans/vm-optimizations-deferred.md`: that one is a perf parking lot
whose entries Claude may pick up inside an approved direction, while
**everything here needs the maintainer to choose first** — these are
spec changes, and several are visible in `README.md`, which is the
language specification.

---

## Exception chaining (Python's `__context__`)

**Maintainer-CONFIRMED (2026-07-09)** that his "nested exceptions" ask
meant CHAINING, not nested `try` blocks — so this is a requested
feature that was scoped and then deliberately deferred, not an idea.

v1 shipped the **C++ default: a `throw` while handling another
exception REPLACES it.** The handler design left room for the chain
(the currently-handled exception is knowable). Three prerequisites,
from `plans/archived/vm-exceptions.md`:

1. a `context` field on the exception object / struct wrapper;
2. the uncaught printer + backtrace walk and render the chain
   (`During handling of the above exception, another exception
   occurred:`);
3. **a surface-syntax decision** — implicit context like Python, or an
   explicit `throw X from Y`.

Not in README.md, not in CLAUDE.md, not in
`plans/exception-object-lifecycle.md`.

## Structs — the four remaining v1 deferrals

CLAUDE.md names these in one line and points at the plan for the
detail, so these sketches are the only written record
(`plans/archived/structs.md` §11).

- **`var` (inferred) fields.** Infer a field's type from ALL
  construction call sites, exactly as function params are inferred
  today: the constructor IS a call, so the field list plugs into the
  same fixpoint contribution (`contribute_arg` over `ctor_params`).
  The blocker is only ORDERING (a struct used before all its
  constructors are seen); a second fixpoint pass over struct fields
  handles it. Today: a hard parse error at parser.cpp.
- **`opt` scalar fields.** Needs a presence representation that does
  not wreck the C layout: either a trailing presence bitmap appended
  to the POD bytes (the struct stays "POD-with-bitmap", still
  flat-array-able) or demoting such a struct to boxed. Both are
  localized to layout + field load/store. Today: `'opt' is only
  allowed on dyn/array/dict fields (v1)`.
- **Methods.** Unbuilt, no design written.
- **Struct SUBTYPING.** `assignable` accepts equal struct types only.
  **This one is in NO other file** — CLAUDE.md's deferral line does
  not mention it, so it vanishes entirely if the archived plan is not
  read.

## `ordered_dict` as a new insertion-ordered type

From `plans/archived/hash-and-dict.md`, filed under "Deferred /
proposed (need the user)": the legitimate version of the "ordered
dict" request — **a new type, not a rename** of the existing unordered
`dict`. Would also pair fairly against Python's `dict`/`OrderedDict`
in benches (today MyLang's unordered iteration order is a documented
divergence in `bench/README.md`). The identifier `ordered_dict`
appears nowhere in the repo.

## Element read out of a BOTTOM container: `none` or `dyn`?

From `plans/archived/value-template-instantiation.md`. The direct-call
`instantiate_round` has a bottom-signature blind spot — this has
always been rejected:

    func v(d) { foreach (k, e in d) print(k + ":"); }
    v({});

The principled fix is to make an element READ out of a bottom
container yield `dyn` rather than `none` — **a lattice change**: it
would turn `var x = empty[0]` into a `DynRequiredEx`, which is a
visible spec change. Needs sign-off before anyone tries it.

## Inline POD in `EvalValue` (I1) — a real semantics fork

From `plans/archived/vm-performance-roadmap.md`. A POD struct <= 16
bytes could live in the 24-byte `EvalValue` payload instead of behind
an `intrusive_ptr` — but that **changes reference semantics** for
those structs (MyLang structs are references today; an inline one
would copy). Filed as design-level: "needs a written semantics
proposal first". The perf motivation is real, the semantics cost is
the whole question.

## A `#ifdef`-style `isdefined()` over a name declared NOWHERE

From `plans/archived/undefined-name-elimination.md`, recorded as a
PARKED IDEA whose "separate future plan" was never written: a query
that can ask about a name declared nowhere at all without a compile
error, which needs EARLY dead-code elimination so the body under the
false branch never has to compile.

**Largely superseded** by #135's Dart-style guarded narrowing (README:
when `x` really does not exist the guarded code is also deleted), but
the early-DCE half was never built. Note CLAUDE.md's only pointer to
this mislabels it "(#135)", which is the landed, DIFFERENT feature —
worth fixing if this is ever picked up.
