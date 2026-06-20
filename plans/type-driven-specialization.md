# Type-driven specialization + mandatory `dyn`

Status: **Phase A + Phase B DONE, including full promotion removal.** Phase A:
`--debug-ti`, the mandatory-`dyn` rule ON by default, and the inference-
completeness audit of the whole corpus. Phase B: type-driven flat-array
representation, then the **complete removal of runtime promotion**. Every
array-producing node is built in its final representation at creation, chosen
from the destination's proven static type via an `ArrHint` the inferencer stamps
(`set_array_repr_hint`, replacing the old `array(N)` auto-fill rewrite) and the
creators honor (`range`/`array`/`make_array`/`LiteralArray`/`LiteralObj`).
`promote_to_general` is **deleted**; `get_vec()`/`get_view()` are general-only;
every op (incl. `insert`/`erase`/`map`/`filter`/`sort`-with-comparator/`dict`-
from-pairs/`join`/`writelines`/spreads) handles flat directly; and the one
residual — mutating a flat array to a non-fitting type through a `dyn` alias —
raises a `TypeError` (`flat_array_violation_msg`) rather than promoting.
**Array element-type strictness (`array<dyn>` requires `dyn`) was deliberately
NOT done** — the user chose tolerant arrays: it forced `dyn` on every
heterogeneous container (literals, mixed dicts, all `kvpairs()` results, ~21
core tests) and was *not* needed for the payoff, since inference already
computes the exact element type. The end goal — representation driven by the
proven static type, not the runtime value, with no specialize-then-promote and
no on-demand promotion — is fully met.

**Phase A results:** every identifier in `bench/my/*` and `samples/*` infers a
concrete type (zero spurious `dyn`); the completeness fixes are the
defer-on-Unknown/None invariant (see CLAUDE.md "Static type inference"), Unknown
func-return/loopvar -> `dyn` finalization, find()->`opt int`, kvpairs typing,
and `str + dyn -> str`. The test corpus gained `dyn` only where genuinely
dynamic. 661/661 tests; bench geomean 0.58x -> 0.50x (more precise types ->
more M8 scalar specialization). That is only sound once the inferencer is
**provably correct in both directions**, so the bulk of the work is hardening
and auditing type inference — gated behind a hard rule that forces the issue.

This supersedes the "deferred type-driven representation" note in
`plans/typed-arrays.md` and the `[[typed-arrays-track]]` memo.

## The end goal (why)

Today array storage is **value-driven** (`plans/typed-arrays.md`, approach B):
`range()`/`array(N,v)`/`make_array` pick flat vs general from the runtime value,
and a flat array that turns out polymorphic just *promotes* (an O(N) cost).
The waste: we specialize arrays we never proved monomorphic, then pay to undo
it.

The fix is **type-driven** representation: emit flat storage **iff** the
inferencer proved `array<int>`/`array<float>` for that value's destination; emit
general for `array<dyn>`. A statically-`array<int>` value can never receive a
non-int element (that's a compile error), so it never promotes — specialization
becomes a pure win, promotion becomes a `dyn`-only escape hatch.

This is only correct if inference is **sound** (never claims `int` where the
real type is `dyn`) **and** we trust it enough to act on it. Hence the rule.

## The forcing rule: `dyn` becomes mandatory

A variable declared with plain `var` (and `const`) **must infer to a fully
concrete static type**. If its inferred type is `dyn` (or contains `dyn` —
pending the array decision below), that is a **compile error**: the user must
declare it `dyn` / `var dyn` to opt into dynamic typing explicitly. There is no
implicit `dyn` anymore.

- Sound concrete types acceptable under plain `var`: `int`, `float`, `str`,
  `opt T` (nullable is a known type), `array<T>`, `dict<K,V>`, function types,
  `Exception`, and (pending decision) `array<dyn>`/`dict<...dyn...>`.
- `dyn` (the top type) anywhere that the rule covers → error → requires `dyn`.
- The error is a compile-time `Exception` (not a `RuntimeException`), like the
  other inferencer diagnostics (`errors.h`), so `try/catch` can't catch it.
- Gated by `-nti` (no inference → no enforcement), on by default otherwise.

This rule makes case (1) below *enforced by construction*: if inference ever
spuriously concludes `dyn`, the build breaks loudly instead of silently
de-optimizing.

## Two correctness properties the inferencer must satisfy

1. **Soundness (no false fix):** never assign a concrete type when a use case
   would force `dyn`. The forcing rule *enforces* this — a wrong `int` that
   should be `dyn` would mis-accept code that must be rejected, surfacing as a
   real type error somewhere. (Already largely true; the audit confirms it.)

2. **Completeness (no false `dyn`):** never infer `dyn` (or `array<dyn>`) when a
   concrete type was provable. This is *not* enforced by anything today — a
   spuriously-`dyn` identifier just silently misses optimization (and, under the
   forcing rule, forces the user to write a needless `dyn`). We need an audit
   tool to find these.

## `--debug-ti`: the audit tool

A new CLI option that runs inference and dumps every identifier's inferred
type + every use site, machine-readable, so we can find and triage `dyn`s.

Proposed format (one record per declared identifier; tab- or `|`-separated, or
JSONL — final format TBD during implementation):

```
name | kind(const|var|param|func) | decl_line | decl_col | isconst | type | uses
```
where `type` renders the full static type (`int`, `opt float`, `array<int>`,
`array<dyn>`, `dict<str,dyn>`, `func(int)->int`, `dyn`, ...) and `uses` is a
list of `line:col` read/write sites (invert the inferencer's `id_sym`
Identifier→TypeSym map; each `Identifier` carries its `start` loc).

Implementation: hang it off `infer_types` (the inferencer already builds scopes,
one `TypeSym` per decl, `id_sym`, and `FuncInfo` per function). A flag threads
in and, after the fixpoint, walks the symbol tables and prints. `-nr`-style:
dump, don't run.

## The audit workflow (the long part)

1. Run `--debug-ti` over **every** script: `src/tests.cpp` entries (extract or a
   harness), `bench/my/*.my`, `samples/*`.
2. Collect every identifier whose `type` is `dyn` or contains `dyn`
   (`array<dyn>`, `dict<_,dyn>`, ...).
3. For each, read its use sites and decide: **justified** (genuinely
   polymorphic — e.g. holds an int then a str, fed by `runtime()`, a real
   heterogeneous array) or a **false `dyn`** (an inference gap).
   - Justified → annotate the script with `dyn` (samples/tests/bench may need
     this; the user expects it).
   - False `dyn` → debug and **fix the inferencer**, add a regression test.
4. Re-run until every remaining `dyn` is justified, then turn the forcing rule
   on and make the whole corpus pass.

## Phasing (split allowed)

- **Phase A — scalars + function return types.** Make inference 100% correct for
  non-array identifiers and function returns; *tolerate* `array<dyn>` (don't
  enforce the rule for array element types yet). Land `--debug-ti`, the forcing
  rule for scalar `dyn`, and the audit of all non-array `dyn`s.
- **Phase B — arrays.** Extend the rule + audit to array element types; fix the
  array inference gaps (the known one: a mixed `append`/store widens
  `array<int>`→`array<dyn>` via `contribute_container`; element-type checking
  vs. widening). Then flip array representation to type-driven (below).

## Type-driven representation mechanics (the payoff, Phase B end) — AS BUILT

Representation is routed off the proven `array<int>`/`array<float>` type:
- The inferencer (`set_array_repr_hint`, in `annotate_hints`, on `a = <rvalue>`
  decls/assigns) stamps a small new `ArrHint` field (`syntax.h`:
  `dflt`/`general`/`flat_i`/`flat_f`) on the array-producing rvalue — on a
  `range()`/`array()`/`make_array()` call's args `ExprList`, or directly on an
  array literal / folded `LiteralObj` — derived from the destination's inferred
  element type. (A separate field, not `TypeHint`: the two are orthogonal.)
- The creators emit flat only when the hint says int/float; a value flowing into
  a `dyn`/`array<dyn>` destination is created general from the start — no
  specialize-then-promote. `range()` (intrinsically `array<int>`) stays flat
  unless its destination is `dyn`. The fixpoint propagates the destination type
  through direct aliases (`var b = a`), so they agree on representation.
- **There is no promotion safety net.** `promote_to_general` is deleted. The one
  case the hint can't cover soundly — mutating a flat array to a non-fitting
  type through a `dyn` alias, where the storage is shared and the static type
  stays `array<int>` — raises a `TypeError` (`flat_array_violation_msg`).
  Declaring the array `dyn` from the start gets a polymorphic (general) array.

## Resolved decisions (confirmed with the user)

1. **`array<dyn>` under plain `var` → STRICT.** Any `dyn` *anywhere* in the
   inferred type (`array<dyn>`, `dict<_,dyn>`, nested, or a bare `dyn`) makes a
   plain `var`/`const` an error; the user must declare it `dyn`/`var dyn`. So
   every plain-`var` array is guaranteed monomorphic `array<T>` (flat-eligible).
2. **Enforcement = HARD compile error, on by default**, disabled by `-nti`.
   `opt T`/`array<T>`/`dict<K,V>`/etc. (any fully-concrete type) are fine; only a
   `dyn` somewhere in the type triggers it. The error is an uncatchable
   compile-time `Exception` (like the other inferencer diagnostics). The corpus
   (tests/bench/samples) gets `dyn` added where genuinely needed.
3. **Function returns:** no return-type annotation syntax. A function whose
   return is genuinely `dyn` is handled at the *caller's* `var` (which then needs
   `dyn`); functions themselves get no new annotation.

## Implementation sequencing

Build the rule behind the inference pass but **flip the default on only after**
`--debug-ti` exists and the corpus is audited+annotated, so development isn't
blocked by a corpus-wide break. End state: on by default.
